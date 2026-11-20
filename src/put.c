#include "tgfs/scan.h"
#include "tgfs/bot_api.h"
#include "internal.h"
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#define PUT_MAX_SIZE (64U*1024U*1024U)
#define PUT_MAX_PARTS 4096U
/* CLI writer is synchronous. Pace mutations; rate limits still remain errors. */
static unsigned send_interval_ms=1100;
static struct timespec last_send;
static int pace(tgfs_error *e)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) return fail(e,"cannot read monotonic clock");
    if(last_send.tv_sec || last_send.tv_nsec) {
        int64_t elapsed=(int64_t)(now.tv_sec-last_send.tv_sec)*1000000000LL+now.tv_nsec-last_send.tv_nsec;
        int64_t wait=(int64_t)send_interval_ms*1000000LL-elapsed;
        if(wait>0) {
            struct timespec delay={.tv_sec=(time_t)(wait/1000000000LL),.tv_nsec=(long)(wait%1000000000LL)};
            while(nanosleep(&delay,&delay)) if(errno!=EINTR) return fail(e,"cannot pace writer requests");
        }
    }
    if(clock_gettime(CLOCK_MONOTONIC,&last_send)) return fail(e,"cannot read monotonic clock");
    return 0;
}
static int same(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static int uuid_new(char text[37])
{
    unsigned char b[16];
    if(RAND_bytes(b,16)!=1) return -1;
    b[6]=(unsigned char)((b[6]&15)|64);b[8]=(unsigned char)((b[8]&63)|128);
    snprintf(text,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);return 0;
}
static int digest(const unsigned char *bytes,size_t length,char hash[65])
{
    unsigned char b[32];unsigned n;
    if(!EVP_Digest(bytes,length,b,&n,EVP_sha256(),NULL) || n!=32) return -1;
    for(size_t i=0;i<32;i++) snprintf(hash+i*2,3,"%02x",b[i]);
    return 0;
}
static int write_file(int dir,const char *name,const unsigned char *bytes,size_t size,tgfs_error *e)
{
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return fail(e,"cannot create staging file");
    size_t done=0;int rc=0;
    while(done<size) {ssize_t n=write(fd,bytes+done,size-done);if(n<=0) {rc=-1;break;}done+=(size_t)n;}
    if(fsync(fd)) rc=-1;
    if(close(fd)) rc=-1;
    return rc ? fail(e,"cannot persist staging file") : 0;
}
static int persist(int dir,cJSON *intent,const char *phase,tgfs_error *e)
{
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"phase");
    if(!cJSON_AddStringToObject(intent,"phase",phase)) return fail(e,"out of memory");
    char *raw=cJSON_PrintUnformatted(intent);
    if(!raw) return fail(e,"out of memory");
    int rc=write_file(dir,"intent.tmp",(const unsigned char *)raw,strlen(raw),e);free(raw);
    if(!rc && (renameat(dir,"intent.tmp",dir,"intent.json") || fsync(dir))) rc=fail(e,"cannot publish intent");
    return rc;
}
static int add_id(cJSON *object,const char *key,long long id)
{
    char text[32];snprintf(text,sizeof(text),"%lld",id);
    return cJSON_AddStringToObject(object,key,text) ? 0 : -1;
}
static cJSON *envelope(const char *type,const char *object)
{
    cJSON *c=cJSON_CreateObject();
    if(!c || !cJSON_AddStringToObject(c,"magic","TGFS") || !cJSON_AddNumberToObject(c,"format_version",1) ||
        !cJSON_AddStringToObject(c,"type",type) || !cJSON_AddStringToObject(c,"object_id",object) ||
        !cJSON_AddNullToObject(c,"next_message")) {cJSON_Delete(c);return NULL;}
    return c;
}
static int api_fail(tgfs_error *e,const tgfs_bot_error *b)
{
    return fail(e,"%s [http=%ld api=%d retry_after=%u outcome_unknown=%d]",
        b->message,b->http_status,b->api_code,b->retry_after,b->outcome_unknown);
}
static int request(tgfs_bot *bot,const char *method,cJSON *p,cJSON **out,tgfs_error *e)
{
    tgfs_bot_error b; cJSON_Delete(*out);*out=NULL;
    int rc=tgfs_bot_call(bot,method,p,out,&b);cJSON_Delete(p);
    return rc ? api_fail(e,&b) : 0;
}
static cJSON *params(const char *chat)
{
    cJSON *p=cJSON_CreateObject();
    if(!p || !cJSON_AddStringToObject(p,"chat_id",chat)) {cJSON_Delete(p);return NULL;}return p;
}
static int check_message(const cJSON *m,long long channel,const char *name,size_t size,const char *caption,long long *id,tgfs_error *e)
{
    long long chat_id,length;
    const cJSON *chat=cJSON_GetObjectItemCaseSensitive(m,"chat"),*doc=cJSON_GetObjectItemCaseSensitive(m,"document");
    if(tgfs_bot_integer(m,"message_id",id) || *id<=0 || tgfs_bot_integer(chat,"id",&chat_id) || chat_id!=channel ||
        !same(json_string(chat,"type"),"channel") || !same(json_string(doc,"file_name"),name) ||
        !json_string(doc,"file_id") || !*json_string(doc,"file_id") ||
        tgfs_bot_integer(doc,"file_size",&length) || length!=(long long)size || !same(json_string(m,"caption"),caption))
        return fail(e,"uploaded/edited document response mismatch; outcome may be unknown");
    return 0;
}
static int upload(tgfs_bot *bot,const char *chat,long long channel,const char *filename,
                  const unsigned char *bytes,size_t size,const cJSON *caption,long long *id,tgfs_error *e,tgfs_bot_error *failure)
{
    if(failure) *failure=(tgfs_bot_error){.kind=TGFS_BOT_LOCAL,.outcome_unknown=1};
    char *text=cJSON_PrintUnformatted(caption);cJSON *out=NULL;
    if(!text) return fail(e,"out of memory");
    tgfs_bot_error b;
    int rc=pace(e);
    if(!rc) {
        rc=tgfs_bot_send_document(bot,chat,filename,bytes,size,text,&out,&b);
        if(rc) {if(failure) *failure=b;api_fail(e,&b);}else rc=check_message(out,channel,filename,size,text,id,e);
    }
    cJSON_Delete(out);free(text);return rc;
}
static int upload_json(tgfs_bot *bot,const char *chat,long long channel,const char *name,
                       const cJSON *body,const cJSON *caption,long long *id,size_t *size,tgfs_error *e,tgfs_bot_error *failure)
{
    if(failure) *failure=(tgfs_bot_error){.kind=TGFS_BOT_LOCAL,.outcome_unknown=1};
    char *raw=cJSON_PrintUnformatted(body);
    if(!raw) return fail(e,"out of memory");
    *size=strlen(raw);
    int rc=upload(bot,chat,channel,name,(const unsigned char *)raw,*size,caption,id,e,failure);free(raw);return rc;
}
static int edit(tgfs_bot *bot,const char *chat,long long channel,long long id,const char *name,
                size_t size,const cJSON *caption,tgfs_error *e)
{
    char *text=cJSON_PrintUnformatted(caption);cJSON *p=params(chat),*out=NULL;
    if(!text || !p || !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddStringToObject(p,"caption",text)) {
        free(text);cJSON_Delete(p);return fail(e,"out of memory");
    }
    int rc=pace(e);long long edited=0;
    if(!rc) rc=request(bot,"editMessageCaption",p,&out,e);else cJSON_Delete(p);
    if(!rc) rc=check_message(out,channel,name,size,text,&edited,e);
    if(!rc && edited!=id) rc=fail(e,"edit returned another message ID");
    free(text);cJSON_Delete(out);return rc;
}
static unsigned char *read_input(const char *path,size_t *size,tgfs_error *e)
{
    char *copy=strdup(path);if(!copy) {fail(e,"out of memory");return NULL;}
    char *slash=strrchr(copy,'/'),*name=copy;
    const char *parent=".";
    if(slash) {name=slash+1;if(slash==copy) parent="/";else {*slash=0;parent=copy;}}
    int dir=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    unsigned char *bytes=NULL;
    if(dir<0) fail(e,"cannot open input parent directory");
    else {bytes=bytes_load_at(dir,name,PUT_MAX_SIZE,size,e);close(dir);}
    free(copy);return bytes;
}
static int check_pin(tgfs_bot *bot,const cJSON *journal,tgfs_error *e)
{
    const char *chat=json_string(journal,"storage_chat_id");cJSON *out=NULL;
    if(request(bot,"getChat",params(chat),&out,e)) return -1;
    const cJSON *pin=cJSON_GetObjectItemCaseSensitive(out,"pinned_message");
    long long id,channel,returned,pin_channel;uint64_t expected;int rc=-1;
    const cJSON *sb=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    const char *raw=json_string(pin,"text");
    cJSON *header=raw?json_parse_bytes((const unsigned char *)raw,strlen(raw),e):NULL;
    if(!header || tgfs_bot_channel_id(chat,&channel) || tgfs_bot_integer(out,"id",&returned) || returned!=channel ||
        !same(json_string(out,"type"),"channel") ||
        tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(pin,"chat"),"id",&pin_channel) || pin_channel!=channel ||
        tgfs_bot_integer(pin,"message_id",&id) || json_decimal(sb,"start_id",&expected) ||
        (uint64_t)id!=expected || !json_semantic_equal(header,sb)) fail(e,"pinned SUPERBLOCK differs from capture");
    else rc=0;
    cJSON_Delete(header);cJSON_Delete(out);return rc;
}

static int stage_open(const char *state,tgfs_error *e)
{
    if(mkdir(state,0700)) {fail(e,"state directory must not exist; preserve previous intent");return -1;}
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(dir<0) {fail(e,"cannot open staging directory");return -1;}
    /* Persist the newly created directory entry before any remote mutation. */
    char *parent=strdup(state);
    if(!parent) {close(dir);fail(e,"out of memory");return -1;}
    size_t parent_length=strlen(parent);
    while(parent_length>1 && parent[parent_length-1]=='/') parent[--parent_length]=0;
    char *slash=strrchr(parent,'/');
    if(slash) {if(slash==parent) slash[1]=0;else *slash=0;}
    else {free(parent);parent=strdup(".");}
    int pfd=parent?open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;free(parent);
    if(pfd<0) {close(dir);fail(e,"cannot open state parent");return -1;}
    int synced=fsync(pfd);close(pfd);if(synced) {close(dir);fail(e,"cannot persist state directory");return -1;}
    return dir;
}

static int caption_parent(cJSON *caption,const char *parent)
{
    return (parent[0] ? cJSON_AddStringToObject(caption,"parent_id",parent) : cJSON_AddNullToObject(caption,"parent_id")) ? 0 : -1;
}
static cJSON *addition(const char *operation,const char *name,const char *object)
{
    cJSON *body=cJSON_CreateObject(),*ops=cJSON_CreateArray(),*entry=cJSON_CreateObject();
    if(!body || !ops || !entry || !cJSON_AddStringToObject(entry,"operation_id",operation) ||
        !cJSON_AddStringToObject(entry,"action","ADD") || !cJSON_AddStringToObject(entry,"name",name) ||
        !cJSON_AddStringToObject(entry,"object_id",object)) goto invalid;
    if(!cJSON_AddItemToArray(ops,entry)) goto invalid;
    entry=NULL;
    if(!cJSON_AddItemToObject(body,"operations",ops)) goto invalid;
    return body;
invalid:
    cJSON_Delete(entry);cJSON_Delete(ops);cJSON_Delete(body);return NULL;
}
static int put(tgfs_bot *bot,const cJSON *journal,const char *file,const char *name,const char *parent_path,
               const char *state,size_t chunk_size,int replace,tgfs_error *e)
{
    int rc=-1,dir=-1,logfd=-1,parts_attached=0;
    unsigned char *bytes=NULL;
    size_t size=0,commit_size=0,directory_size=0;
    cJSON *intent=NULL,*parts=NULL,*captions=NULL,*commit=NULL,*body=NULL,*directory=NULL,*directory_body=NULL;
    const char *chat=json_string(journal,"storage_chat_id");long long channel=0,commit_id=0,directory_id=0;
    char parent_id[37],grandparent[37],base_version[37],object[37];uint64_t base_commit=0;
    cJSON *wire=cJSON_CreateString(name);char *encoded=wire?cJSON_PrintUnformatted(wire):NULL;
    cJSON *decoded=encoded?json_parse_bytes((const unsigned char *)encoded,strlen(encoded),e):NULL;
    cJSON_Delete(wire);free(encoded);
    if(!decoded) {fail(e,"filename must be valid UTF-8");goto done;}cJSON_Delete(decoded);
    int target=replace ? remote_replacement_target(bot,journal,parent_path,name,object,parent_id,base_version,&base_commit,e) :
        remote_directory_target(bot,journal,parent_path,name,parent_id,grandparent,e);
    if(target || tgfs_bot_channel_id(chat,&channel) || !uuid_valid(parent_id) || check_pin(bot,journal,e)) goto done;
    bytes=read_input(file,&size,e);if(!bytes) goto done;
    size_t count=(size+chunk_size-1)/chunk_size;
    if(count>PUT_MAX_PARTS) {fail(e,"too many parts for put prototype");goto done;}
    uint64_t boundary;
    if(json_decimal(journal,"boundary_id",&boundary)) {fail(e,"invalid captured boundary");goto done;}
    long long previous=(long long)boundary;
    char version[37],operation[37],add[37],full_hash[65];
    if((!replace && (uuid_new(object) || uuid_new(add))) || uuid_new(version) || uuid_new(operation) || digest(bytes,size,full_hash)) {
        fail(e,"cannot generate publication IDs or SHA-256");goto done;
    }
    dir=stage_open(state,e);if(dir<0) goto done;
    logfd=openat(dir,"backlink-repair.log",O_RDWR|O_CREAT|O_EXCL|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat logstat;
    if(logfd<0 || fstat(logfd,&logstat) || !S_ISREG(logstat.st_mode) || logstat.st_nlink!=1 ||
        logstat.st_uid!=geteuid() || (logstat.st_mode&0077) || flock(logfd,LOCK_EX|LOCK_NB) || fsync(dir)) {
        fail(e,"cannot create and lock private put recovery journal");goto done;
    }
    if(write_file(dir,"data.bin",bytes,size,e)) goto done;
    intent=cJSON_CreateObject();parts=cJSON_CreateArray();captions=cJSON_CreateArray();
    commit=envelope("FILE_COMMIT",object);body=cJSON_CreateObject();
    if(!replace) {directory=envelope("DIRECTORY",parent_id);directory_body=addition(add,name,object);}
    if(!intent || !parts || !captions || !commit || !body || (!replace && (!directory || !directory_body)) ||
        !cJSON_AddStringToObject(intent,"kind","put") || !cJSON_AddStringToObject(intent,"storage_chat_id",chat) || !cJSON_AddStringToObject(intent,"parent_id",parent_id) ||
        !cJSON_AddStringToObject(intent,"source_boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddStringToObject(intent,"parent_path",parent_path) || !cJSON_AddStringToObject(intent,"name",name) || !cJSON_AddStringToObject(intent,"object_id",object) ||
        !cJSON_AddStringToObject(intent,"version_id",version) || !cJSON_AddStringToObject(intent,"operation_id",operation) ||
        !cJSON_AddBoolToObject(intent,"replace",replace) || !cJSON_AddStringToObject(intent,"file_sha256",full_hash) ||
        add_id(intent,"file_size",(long long)size) || !cJSON_AddNumberToObject(intent,"chunk_bytes",(double)chunk_size) ||
        !cJSON_AddItemToObject(intent,"parts",parts)) {fail(e,"cannot allocate intent");goto done;}
    parts_attached=1;
    if(!cJSON_AddStringToObject(commit,"version_id",version) || !cJSON_AddStringToObject(commit,"operation_id",operation) ||
        !cJSON_AddStringToObject(commit,"parent_id",parent_id) || !cJSON_AddStringToObject(commit,"file_sha256",full_hash) ||
        !cJSON_AddNumberToObject(commit,"part_count",(double)count) || add_id(commit,"file_size",(long long)size) ||
        (!replace && caption_parent(directory,grandparent))) {fail(e,"out of memory");goto done;}
    if((replace && (!cJSON_AddStringToObject(intent,"base_version_id",base_version) || add_id(intent,"base_commit_id",(long long)base_commit))) ||
        (!replace && (!cJSON_AddStringToObject(intent,"add_operation_id",add) ||
        !cJSON_AddItemToObject(intent,"directory_caption",cJSON_Duplicate(directory,1)) ||
        !cJSON_AddItemToObject(intent,"directory_document",cJSON_Duplicate(directory_body,1)))) ||
        !cJSON_AddItemToObject(intent,"commit_caption",cJSON_Duplicate(commit,1))) {fail(e,"out of memory");goto done;}
    if(persist(dir,intent,"prepared",e)) goto done;
    for(size_t i=0;i<count;i++) {
        size_t offset=i*chunk_size,length=size-offset;if(length>chunk_size) length=chunk_size;
        char hash[65],filename[256];snprintf(filename,sizeof(filename),"%s.%zu",name,i);
        cJSON *caption=envelope("CHUNK",object),*part=cJSON_CreateObject();
        if(digest(bytes+offset,length,hash) || !caption || !part ||
            !cJSON_AddStringToObject(caption,"parent_id",parent_id) || !cJSON_AddStringToObject(caption,"version_id",version) ||
            !cJSON_AddNumberToObject(caption,"part_count",(double)count) || add_id(caption,"offset",(long long)offset) ||
            add_id(caption,"part_size",(long long)length) || add_id(caption,"file_size",(long long)size) ||
            !cJSON_AddStringToObject(caption,"part_sha256",hash) || !cJSON_AddStringToObject(caption,"file_sha256",full_hash) ||
            !cJSON_AddNullToObject(caption,"file_commit_id") ||
            add_id(part,"offset",(long long)offset) || add_id(part,"part_size",(long long)length) ||
            !cJSON_AddStringToObject(part,"part_sha256",hash)) {
            cJSON_Delete(caption);cJSON_Delete(part);fail(e,"out of memory");goto done;
        }
        if(!cJSON_AddItemToArray(captions,caption)) {cJSON_Delete(caption);cJSON_Delete(part);fail(e,"out of memory");goto done;}
        cJSON_DeleteItemFromObjectCaseSensitive(intent,"last_attempted_part");
        char index[32];snprintf(index,sizeof(index),"%zu",i);
        if(!cJSON_AddNumberToObject(intent,"last_attempted_part",(double)i) || !cJSON_AddNumberToObject(intent,"pending_part",(double)i) ||
            upload_request_event(logfd,"upload_chunk_pending",index,operation,e) || persist(dir,intent,"chunk_send_pending",e)) {
            cJSON_Delete(part);goto done;
        }
        long long id=0;
        tgfs_bot_error rejection;
        int uploaded=upload(bot,chat,channel,filename,bytes+offset,length,caption,&id,e,&rejection);
        if(uploaded && upload_rejected(&rejection)) {
            tgfs_error saved=*e;
            if(!upload_rejection(logfd,"upload_chunk_rejected",index,operation,&rejection,e)) *e=saved;
            cJSON_Delete(part);goto done;
        }
        if(!uploaded && id<=previous) uploaded=fail(e,"non-increasing uploaded message ID");
        if(!uploaded) previous=id;
        /* A known ID is retained even if remaining response metadata mismatched. */
        if(id>0 && add_id(part,"message_id",id)) {cJSON_Delete(part);fail(e,"out of memory");goto done;}
        if(id>0) {
            if(!cJSON_AddItemToArray(parts,part)) {cJSON_Delete(part);fail(e,"out of memory");goto done;}
            part=NULL;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(intent,"pending_part");
        cJSON_Delete(part);
        if(uploaded) {tgfs_error saved=*e;persist(dir,intent,"chunk_response_uncertain",e);*e=saved;goto done;}
        if(persist(dir,intent,"chunk_sent",e)) goto done;
    }
    if(replace && check_pin(bot,journal,e)) goto done;
    if(!cJSON_AddItemToObject(body,"parts",cJSON_Duplicate(parts,1)) ||
        !cJSON_AddItemToObject(intent,"commit_document",cJSON_Duplicate(body,1)) ||
        upload_request_event(logfd,"upload_commit_pending",operation,operation,e) || persist(dir,intent,"commit_send_pending",e)) goto done;
    tgfs_bot_error rejection;
    int committed=upload_json(bot,chat,channel,"file-commit.json",body,commit,&commit_id,&commit_size,e,&rejection);
    if(committed && upload_rejected(&rejection)) {
        tgfs_error saved=*e;
        if(!upload_rejection(logfd,"upload_commit_rejected",operation,operation,&rejection,e)) *e=saved;
        goto done;
    }
    if(!committed && commit_id<=previous) committed=fail(e,"non-increasing commit ID");
    if(commit_id>0 && add_id(intent,"file_commit_id",commit_id)) {fail(e,"out of memory");goto done;}
    if(committed) {tgfs_error saved=*e;persist(dir,intent,"commit_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,replace?"published_backlinks_pending":"backlinks_pending",e)) goto done;
    for(size_t i=0;i<count;i++) {
        cJSON *caption=cJSON_GetArrayItem(captions,(int)i);
        const cJSON *part=cJSON_GetArrayItem(parts,(int)i);uint64_t mid,next;
        if(json_decimal(part,"message_id",&mid)) {fail(e,"invalid local CHUNK ID");goto done;}
        if(i+1<count) {
            if(json_decimal(cJSON_GetArrayItem(parts,(int)i+1),"message_id",&next)) {fail(e,"invalid next CHUNK ID");goto done;}
        } else next=(uint64_t)commit_id;
        cJSON_DeleteItemFromObjectCaseSensitive(caption,"file_commit_id");cJSON_DeleteItemFromObjectCaseSensitive(caption,"next_message");
        if(add_id(caption,"file_commit_id",commit_id) || add_id(caption,"next_message",(long long)next)) {fail(e,"out of memory");goto done;}
        char filename[256];snprintf(filename,sizeof(filename),"%s.%zu",name,i);
        uint64_t part_size;if(json_decimal(part,"part_size",&part_size)) {fail(e,"invalid local part size");goto done;}
        if(edit(bot,chat,channel,(long long)mid,filename,(size_t)part_size,caption,e)) goto done;
    }
    if(!replace) {
        if(check_pin(bot,journal,e) || upload_log_event(logfd,"directory_send_pending",add,e) ||
            persist(dir,intent,"directory_send_pending",e)) goto done;
        int published=upload_json(bot,chat,channel,"directory.json",directory_body,directory,&directory_id,&directory_size,e,&rejection);
        if(published && upload_rejected(&rejection)) {
            tgfs_error saved=*e;
            if(!upload_rejection(logfd,"directory_send_rejected",add,add,&rejection,e)) *e=saved;
            goto done;
        }
        (void)directory_size;
        if(!published && directory_id<=commit_id) published=fail(e,"non-increasing directory ID");
        if(directory_id>0 && add_id(intent,"directory_message_id",directory_id)) {fail(e,"out of memory");goto done;}
        if(published) {tgfs_error saved=*e;persist(dir,intent,"directory_response_uncertain",e);*e=saved;goto done;}
        if(persist(dir,intent,"published_navigation_pending",e)) goto done;
        cJSON_DeleteItemFromObjectCaseSensitive(commit,"next_message");
        if(add_id(commit,"next_message",directory_id) || edit(bot,chat,channel,commit_id,"file-commit.json",commit_size,commit,e)) goto done;
    }
    if(persist(dir,intent,"complete",e)) goto done;
    printf("OK put name=%s object_id=%s parts=%zu bytes=%zu commit_id=%lld directory_id=%lld replaced=%d\n",
        name,object,count,size,commit_id,directory_id,replace);rc=0;
done:
    if(rc && intent) {
        const cJSON *last=cJSON_GetArrayItem(parts,cJSON_GetArraySize(parts)-1);
        fprintf(stderr,"put state: phase=%s commit_id=%lld directory_id=%lld last_chunk_id=%s\n",
            json_string(intent,"phase")?json_string(intent,"phase"):"preparing",commit_id,directory_id,
            json_string(last,"message_id")?json_string(last,"message_id"):"unknown");
    }
    if(logfd>=0) close(logfd);
    if(dir>=0) close(dir);
    if(!parts_attached) cJSON_Delete(parts);
    free(bytes);cJSON_Delete(intent);cJSON_Delete(captions);cJSON_Delete(commit);cJSON_Delete(body);
    cJSON_Delete(directory);cJSON_Delete(directory_body);
    /* parts is borrowed once attached to intent, otherwise still owned here. */
    return rc;
}
void tgfs_put_help(FILE *stream)
{
    fputs("usage: tgfs put --capture DIRECTORY --file PATH --name NAME --state-dir NEW_DIRECTORY --token-file PATH [options]\n"
          "Create a file (CHUNK -> FILE_COMMIT -> ADD), or replace its contents explicitly.\n"
          "  --capture DIRECTORY      Completed journal; root pin must still match.\n"
          "  --file PATH              Stable regular input file, max 64 MiB; empty is allowed.\n"
          "  --name NAME              Target filename, max 245 bytes (no path separators).\n"
          "  --replace                Require an existing file, keep object_id, publish a new version.\n"
          "  --parent /absolute/path  Existing captured directory (default /).\n"
          "  --state-dir DIRECTORY    New private staging/intent directory; preserve on failure.\n"
          "  --token-file PATH        Owner-only token file for the bot used by scan.\n"
          "  --chunk-bytes N          1..20971520, default 20971520; max 4096 parts.\n"
          "  --proxy URL | --direct   Same proxy settings as read.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --send-interval-ms N     Minimum mutation interval, 0..10000, default 1100.\n"
          "  -h, --help               Show help without network.\n"
          "No automatic resume/retry. Concurrent replacements use the latest published FILE_COMMIT.\n",stream);
}
int tgfs_put_main(int argc,char **argv)
{
    const char *parent_path="/";int parent_seen=0,replace=0;
    const char *capture=NULL,*file=NULL,*name=NULL,*state=NULL,*token=NULL,*proxy=NULL,*timeout_text=NULL,*chunk_text=NULL,*interval_text=NULL;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_put_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--replace")) {if(replace) goto usage;replace=1;continue;}
        if(same(argv[i],"--parent")) {if(parent_seen || i+1>=argc) goto usage;parent_seen=1;parent_path=argv[++i];continue;}
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--capture")) slot=&capture;
        else if(same(argv[i],"--file")) slot=&file;
        else if(same(argv[i],"--name")) slot=&name;
        else if(same(argv[i],"--state-dir")) slot=&state;
        else if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        else if(same(argv[i],"--chunk-bytes")) slot=&chunk_text;
        else if(same(argv[i],"--send-interval-ms")) slot=&interval_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60,chunk=TGFS_MAX_CHUNK,interval=1100;
    if(!capture || !file || !parent_path || parent_path[0]!='/' || strlen(parent_path)>4096 || !name_valid(name) || strchr(name,'\r') || strchr(name,'\n') || strlen(name)>245 || !state || !*state || !token ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300)) ||
        (chunk_text && (decimal(chunk_text,&chunk) || !chunk || chunk>TGFS_MAX_CHUNK)) ||
        (interval_text && (decimal(interval_text,&interval) || interval>10000))) goto usage;
    send_interval_ms=(unsigned)interval;last_send=(struct timespec){0};
    tgfs_error e={0};tgfs_bot_error b;tgfs_bot *bot=NULL;cJSON *journal=NULL;int rc=1;
    int dir=open(capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {fail(&e,"cannot open capture");goto done;}
    journal=json_load_at(dir,"journal.json",&e);close(dir);if(!journal) goto done;
    if(tgfs_bot_open(token,(unsigned)timeout,&bot,&b) || tgfs_bot_set_proxy(bot,proxy,&b)) {api_fail(&e,&b);goto done;}
    rc=put(bot,journal,file,name,parent_path,state,(size_t)chunk,replace,&e) ? 1 : 0;
done:
    if(rc) fprintf(stderr,"FAIL put: %s; preserve state directory\n",e.message);
    cJSON_Delete(journal);tgfs_bot_close(bot);return rc;
usage:
    tgfs_put_help(stderr);return 2;
}

static int make_directory(tgfs_bot *bot,const cJSON *journal,const char *path,const char *state,tgfs_error *e)
{
    int rc=-1,dir=-1;char *split=strdup(path);
    cJSON *intent=NULL,*child=NULL,*empty=NULL,*parent_caption=NULL,*body=NULL;
    long long child_id=0,parent_id=0,channel;size_t child_size=0,parent_size=0;
    if(!split) return fail(e,"out of memory");
    size_t length=strlen(split);
    while(length>1 && split[length-1]=='/') split[--length]=0;
    char *slash=strrchr(split,'/');const char *name=slash+1;
    const char *parent_path="/";
    if(slash!=split) {*slash=0;parent_path=split;}
    char parent[37],grandparent[37],object[37],operation[37];uint64_t boundary;
    const char *chat=json_string(journal,"storage_chat_id");
    /* Parsing our JSON string also validates UTF-8 before any remote mutation. */
    cJSON *wire=cJSON_CreateString(name);char *raw=wire?cJSON_PrintUnformatted(wire):NULL;
    cJSON *decoded=raw?json_parse_bytes((const unsigned char *)raw,strlen(raw),e):NULL;
    cJSON_Delete(wire);free(raw);
    if(!decoded) {fail(e,"directory name must be valid UTF-8");goto done;}cJSON_Delete(decoded);
    if(remote_directory_target(bot,journal,parent_path,name,parent,grandparent,e) ||
        tgfs_bot_channel_id(chat,&channel) || check_pin(bot,journal,e)) goto done;
    if(json_decimal(journal,"boundary_id",&boundary) || uuid_new(object) || uuid_new(operation)) {
        fail(e,"cannot prepare directory publication");goto done;
    }
    dir=stage_open(state,e);if(dir<0) goto done;
    intent=cJSON_CreateObject();child=envelope("DIRECTORY",object);empty=cJSON_CreateObject();
    parent_caption=envelope("DIRECTORY",parent);body=addition(operation,name,object);
    if(!intent || !child || !empty || !parent_caption || !body || caption_parent(child,parent) ||
        caption_parent(parent_caption,grandparent) || !cJSON_AddArrayToObject(empty,"operations") ||
        !cJSON_AddStringToObject(intent,"kind","mkdir") || !cJSON_AddStringToObject(intent,"storage_chat_id",chat) ||
        !cJSON_AddStringToObject(intent,"source_boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddStringToObject(intent,"path",path) || !cJSON_AddStringToObject(intent,"parent_id",parent) ||
        !cJSON_AddStringToObject(intent,"object_id",object) || !cJSON_AddStringToObject(intent,"add_operation_id",operation) ||
        !cJSON_AddItemToObject(intent,"child_caption",cJSON_Duplicate(child,1)) ||
        !cJSON_AddItemToObject(intent,"child_document",cJSON_Duplicate(empty,1)) ||
        !cJSON_AddItemToObject(intent,"directory_caption",cJSON_Duplicate(parent_caption,1)) ||
        !cJSON_AddItemToObject(intent,"directory_document",cJSON_Duplicate(body,1))) {
        fail(e,"cannot allocate directory intent");goto done;
    }
    if(persist(dir,intent,"child_send_pending",e)) goto done;
    int sent=upload_json(bot,chat,channel,"directory.json",empty,child,&child_id,&child_size,e,NULL);
    if(!sent && (uint64_t)child_id<=boundary) sent=fail(e,"non-increasing child directory ID");
    if(child_id>0 && add_id(intent,"child_message_id",child_id)) {fail(e,"out of memory");goto done;}
    if(sent) {tgfs_error saved=*e;persist(dir,intent,"child_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,"child_sent",e) || check_pin(bot,journal,e) || persist(dir,intent,"directory_send_pending",e)) goto done;
    sent=upload_json(bot,chat,channel,"directory.json",body,parent_caption,&parent_id,&parent_size,e,NULL);
    (void)parent_size;
    if(!sent && parent_id<=child_id) sent=fail(e,"non-increasing parent directory ID");
    if(parent_id>0 && add_id(intent,"directory_message_id",parent_id)) {fail(e,"out of memory");goto done;}
    if(sent) {tgfs_error saved=*e;persist(dir,intent,"directory_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,"published_navigation_pending",e)) goto done;
    cJSON_DeleteItemFromObjectCaseSensitive(child,"next_message");
    if(add_id(child,"next_message",parent_id) || edit(bot,chat,channel,child_id,"directory.json",child_size,child,e) ||
        persist(dir,intent,"complete",e)) goto done;
    printf("OK mkdir path=%s object_id=%s child_id=%lld directory_id=%lld\n",path,object,child_id,parent_id);rc=0;
done:
    if(rc && intent) fprintf(stderr,"mkdir state: phase=%s child_id=%lld directory_id=%lld\n",
        json_string(intent,"phase")?json_string(intent,"phase"):"preparing",child_id,parent_id);
    if(dir>=0) close(dir);
    free(split);cJSON_Delete(intent);cJSON_Delete(child);cJSON_Delete(empty);cJSON_Delete(parent_caption);cJSON_Delete(body);return rc;
}
static int unlink_file(tgfs_bot *bot,const cJSON *journal,const char *path,const char *state,tgfs_error *e)
{
    int rc=-1,dir=-1;char *split=strdup(path);
    cJSON *observed=NULL,*intent=NULL,*caption=NULL,*body=NULL;
    long long channel,directory_id=0;size_t document_size=0;uint64_t boundary;
    if(!split) return fail(e,"out of memory");
    char *slash=strrchr(split,'/');const char *name=slash+1,*parent_path="/";
    if(slash!=split) {*slash=0;parent_path=split;}
    char object[37],parent[37],grandparent[37],operation[37];
    const char *chat=json_string(journal,"storage_chat_id");
    if(remote_unlink_target(bot,journal,parent_path,name,object,parent,grandparent,&observed,e) ||
        tgfs_bot_channel_id(chat,&channel) || check_pin(bot,journal,e)) goto done;
    if(json_decimal(journal,"boundary_id",&boundary) || uuid_new(operation)) {fail(e,"cannot prepare unlink");goto done;}
    dir=stage_open(state,e);if(dir<0) goto done;
    intent=cJSON_CreateObject();caption=envelope("DIRECTORY",parent);body=cJSON_CreateObject();
    cJSON *ops=body?cJSON_AddArrayToObject(body,"operations"):NULL;
    cJSON *entry=ops?cJSON_CreateObject():NULL;
    if(!entry || !cJSON_AddItemToArray(ops,entry)) {cJSON_Delete(entry);fail(e,"out of memory");goto done;}
    if(!cJSON_AddStringToObject(entry,"operation_id",operation) || !cJSON_AddStringToObject(entry,"action","REMOVE") ||
        !cJSON_AddStringToObject(entry,"name",name) || !cJSON_AddItemToObject(entry,"observed_adds",cJSON_Duplicate(observed,1)) ||
        !intent || !caption || caption_parent(caption,grandparent) ||
        !cJSON_AddStringToObject(intent,"kind","unlink") || !cJSON_AddStringToObject(intent,"path",path) ||
        !cJSON_AddStringToObject(intent,"storage_chat_id",chat) || !cJSON_AddStringToObject(intent,"parent_id",parent) ||
        !cJSON_AddStringToObject(intent,"object_id",object) || !cJSON_AddStringToObject(intent,"remove_operation_id",operation) ||
        !cJSON_AddStringToObject(intent,"source_boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddItemToObject(intent,"observed_adds",cJSON_Duplicate(observed,1)) ||
        !cJSON_AddItemToObject(intent,"directory_caption",cJSON_Duplicate(caption,1)) ||
        !cJSON_AddItemToObject(intent,"directory_document",cJSON_Duplicate(body,1))) {fail(e,"cannot allocate unlink intent");goto done;}
    if(persist(dir,intent,"prepared",e) || check_pin(bot,journal,e) || persist(dir,intent,"directory_send_pending",e)) goto done;
    int sent=upload_json(bot,chat,channel,"directory.json",body,caption,&directory_id,&document_size,e,NULL);
    if(!sent && (uint64_t)directory_id<=boundary) sent=fail(e,"non-increasing DIRECTORY ID");
    if(directory_id>0 && add_id(intent,"directory_message_id",directory_id)) {fail(e,"out of memory");goto done;}
    if(sent) {tgfs_error saved=*e;persist(dir,intent,"directory_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,"complete",e)) goto done;
    printf("OK unlink path=%s object_id=%s directory_id=%lld operation_id=%s\n",path,object,directory_id,operation);rc=0;
done:
    if(rc && intent) fprintf(stderr,"unlink state: phase=%s directory_id=%lld\n",
        json_string(intent,"phase")?json_string(intent,"phase"):"preparing",directory_id);
    if(dir>=0) close(dir);
    free(split);cJSON_Delete(observed);cJSON_Delete(intent);cJSON_Delete(caption);cJSON_Delete(body);return rc;
}
void tgfs_unlink_help(FILE *stream)
{
    fputs("usage: tgfs unlink --capture DIRECTORY --path /absolute/file --state-dir NEW_DIRECTORY --token-file PATH [options]\n"
          "Remove a file name by publishing DIRECTORY REMOVE for captured live ADD IDs.\n"
          "  --capture DIRECTORY      Completed journal with DIRECTORY operations.\n"
          "  --path /absolute/file    Existing regular file; no trailing slash.\n"
          "  --state-dir DIRECTORY    New private intent directory; preserve on failure.\n"
          "  --token-file PATH        Owner-only token file for the bot used by scan.\n"
          "  --proxy URL | --direct   Same transport routing as put.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --send-interval-ms N     0..10000, default 1100.\n"
          "  -h, --help               Show help without network.\n"
          "No physical deletion, rmdir, legacy entries conversion, resume or blind retry.\n",stream);
}
void tgfs_mkdir_help(FILE *stream)
{
    fputs("usage: tgfs mkdir --capture DIRECTORY --path /absolute/path --state-dir NEW_DIRECTORY --token-file PATH [options]\n"
          "Create one directory: empty DIRECTORY -> parent DIRECTORY ADD. Parent must exist in capture.\n"
          "  --capture DIRECTORY      Completed scan journal.\n"
          "  --path /absolute/path    New directory, final name up to 255 bytes. No mkdir -p.\n"
          "  --state-dir DIRECTORY    New private intent directory; preserve on failure.\n"
          "  --token-file PATH        Owner-only token file for the bot used by scan.\n"
          "  --proxy URL | --direct   Same transport routing as put.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --send-interval-ms N     0..10000, default 1100.\n"
          "  -h, --help               Show help without network.\n"
          "No overwrite, resume or blind retries. Scan again before using the new directory as parent.\n",stream);
}
static int path_write_main(int argc,char **argv,int removing)
{
    void (*usage_help)(FILE *)=removing?tgfs_unlink_help:tgfs_mkdir_help;
    const char *command=removing?"unlink":"mkdir";
    const char *capture=NULL,*path=NULL,*state=NULL,*token=NULL,*proxy=NULL,*timeout_text=NULL,*interval_text=NULL;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {usage_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--capture")) slot=&capture;
        else if(same(argv[i],"--path")) slot=&path;
        else if(same(argv[i],"--state-dir")) slot=&state;
        else if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        else if(same(argv[i],"--send-interval-ms")) slot=&interval_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60,interval=1100;
    if(!capture || !path || path[0]!='/' || strlen(path)>4096 || !state || !*state || !token ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300)) ||
        (interval_text && (decimal(interval_text,&interval) || interval>10000))) goto usage;
    char *check=strdup(path);if(!check) {fprintf(stderr,"FAIL %s: out of memory\n",command);return 1;}
    size_t n=strlen(check);while(!removing && n>1 && check[n-1]=='/') check[--n]=0;
    const char *name=strrchr(check,'/')+1;
    int valid=name_valid(name) && strlen(name)<=255 && !strchr(name,'\r') && !strchr(name,'\n');free(check);
    if(!valid) goto usage;
    send_interval_ms=(unsigned)interval;last_send=(struct timespec){0};
    tgfs_error e={0};tgfs_bot_error b;tgfs_bot *bot=NULL;cJSON *journal=NULL;int rc=1;
    int dir=open(capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {fail(&e,"cannot open capture");goto done;}
    journal=json_load_at(dir,"journal.json",&e);close(dir);if(!journal) goto done;
    if(tgfs_bot_open(token,(unsigned)timeout,&bot,&b) || tgfs_bot_set_proxy(bot,proxy,&b)) {api_fail(&e,&b);goto done;}
    rc=(removing?unlink_file(bot,journal,path,state,&e):make_directory(bot,journal,path,state,&e)) ? 1 : 0;
done:
    if(rc) fprintf(stderr,"FAIL %s: %s; preserve state directory\n",command,e.message);
    cJSON_Delete(journal);tgfs_bot_close(bot);return rc;
usage:
    usage_help(stderr);return 2;
}

int tgfs_mkdir_main(int argc,char **argv) {return path_write_main(argc,argv,0);}
int tgfs_unlink_main(int argc,char **argv) {return path_write_main(argc,argv,1);}

/* Resume only from evidence: a missing captured message never proves an unsent request. */
int resume_upload(tgfs_bot *bot,tgfs *fs,const char *state,const cJSON *journal,const cJSON *original,cJSON *report,int logfd,tgfs_error *e)
{
    if(!same(json_string(original,"kind"),"put") || !same(json_string(report,"staging"),"verified"))
        return fail(e,"upload resume requires verified put staging");
    if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"ambiguous_part_indexes")) ||
        cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"unobserved_known_ids")))
        return fail(e,"upload resume blocked by ambiguous or unobserved known parts");
    if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"commit_ids")))
        return cJSON_AddStringToObject(report,"upload_resume","already_committed")?0:fail(e,"out of memory");
    if(!json_string(original,"parent_path") || json_string(original,"parent_path")[0]!='/' ||
        !json_string(original,"name") || strlen(json_string(original,"name"))>245) return fail(e,"invalid saved upload destination");
    const char *phase=json_string(original,"phase");int commit_retry=same(phase,"commit_send_pending");
    if(!commit_retry && !same(phase,"prepared") && !same(phase,"chunk_sent") && !same(phase,"chunk_send_pending") && !same(phase,"chunk_response_uncertain"))
        return fail(e,"FILE_COMMIT may already have been attempted; capture and review its outcome");
    if(cJSON_GetObjectItemCaseSensitive(original,"file_commit_id") || (!commit_retry && cJSON_GetObjectItemCaseSensitive(original,"commit_document")) ||
        cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"directory_ids"))) return fail(e,"early upload intent contains publication evidence");
    const cJSON *record;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *c=cJSON_GetObjectItemCaseSensitive(record,"record");
        if(same(json_string(c,"type"),"FILE_COMMIT") &&
            (same(json_string(c,"operation_id"),json_string(original,"operation_id")) ||
             (same(json_string(c,"object_id"),json_string(original,"object_id")) && same(json_string(c,"version_id"),json_string(original,"version_id")))))
            return fail(e,"captured commit requires publication reconciliation, never resend blindly");
    }
    uint64_t size,boundary;size_t chunk,count;long long channel;
    if(tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&channel)) return fail(e,"invalid storage channel");
    if(json_decimal(original,"file_size",&size) || json_decimal(journal,"boundary_id",&boundary) ||
        json_count(original,"chunk_bytes",&chunk) || !chunk || chunk>TGFS_MAX_CHUNK || size>PUT_MAX_SIZE)
        return fail(e,"invalid upload metadata");
    count=(size_t)((size+chunk-1)/chunk);
    if(count>PUT_MAX_PARTS) return fail(e,"too many upload parts");
    const cJSON *oldparts=cJSON_GetObjectItemCaseSensitive(original,"parts");size_t known=(size_t)cJSON_GetArraySize(oldparts);
    unsigned char attempted[4096]={0};int commit_attempted=0;
    if(upload_attempts(logfd,json_string(original,"operation_id"),attempted,&commit_attempted,e)) return -1;
    if(commit_retry ? (commit_attempted!=2 || known!=count || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(original,"commit_document"))) : commit_attempted!=0)
        return fail(e,"FILE_COMMIT retry requires durable rejection of the saved complete manifest");
    size_t retry_index=SIZE_MAX;
    for(size_t i=0;i<4096;i++) {
        if(i>=count && attempted[i]) return fail(e,"logged part outside saved upload");
        if(attempted[i]==2) {
            if(!same(phase,"chunk_send_pending") || i!=known) return fail(e,"rejected CHUNK disagrees with saved pending frontier");
            retry_index=i;attempted[i]=0;
        }
    }
    if(known>count || (same(phase,"prepared") && known) || (same(phase,"chunk_sent") && !known))
        return fail(e,"invalid saved upload frontier");
    for(size_t i=0;i<known;i++) {
        uint64_t offset;
        if(json_decimal(cJSON_GetArrayItem(oldparts,(int)i),"offset",&offset) || offset!=i*chunk)
            return fail(e,"saved part refs must form a contiguous prefix");
        attempted[i]=1;
    }
    if(same(phase,"chunk_send_pending")) {
        size_t pending;
        if(json_count(original,"pending_part",&pending) || pending!=known || pending>=count) return fail(e,"invalid pending part frontier");
        if(pending!=retry_index) attempted[pending]=1;
    } else if(same(phase,"chunk_response_uncertain")) {
        size_t last;
        if(cJSON_GetObjectItemCaseSensitive(original,"last_attempted_part")) {
            if(json_count(original,"last_attempted_part",&last) || last>=count || (last!=known && last+1!=known))
                return fail(e,"invalid last attempted part");
            attempted[last]=1;
        } else if(known<count) attempted[known]=1; /* Legacy intent: conservatively include the next index. */
    }
    const cJSON *found[4096]={0};
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *c=cJSON_GetObjectItemCaseSensitive(record,"record");
        if(!same(json_string(c,"type"),"CHUNK") || !same(json_string(c,"object_id"),json_string(original,"object_id")) ||
            !same(json_string(c,"version_id"),json_string(original,"version_id"))) continue;
        uint64_t offset;
        if(json_decimal(c,"offset",&offset) || offset/chunk>=count) return fail(e,"invalid captured upload range");
        found[offset/chunk]=record;
    }
    int gap=0;uint64_t previous=0;
    for(size_t i=0;i<count;i++) {
        if(!found[i]) {
            gap=1;
            if(attempted[i]) return fail(e,"part %zu may already have been attempted; missing capture evidence blocks resend",i);
        } else {
            uint64_t id;
            if(gap || json_decimal(found[i],"message_id",&id) || id<=previous) return fail(e,"captured upload parts must form an ordered prefix");
            previous=id;
            if(i<known && !same(json_string(cJSON_GetArrayItem(oldparts,(int)i),"message_id"),json_string(found[i],"message_id")))
                return fail(e,"saved and captured part identities differ");
        }
    }
    const cJSON *cc=cJSON_GetObjectItemCaseSensitive(original,"commit_caption");size_t format;
    if(!uuid_valid(json_string(original,"operation_id")) || !same(json_string(cc,"operation_id"),json_string(original,"operation_id")) ||
        !same(json_string(cc,"magic"),"TGFS") || !same(json_string(cc,"type"),"FILE_COMMIT") ||
        json_count(cc,"format_version",&format) || format!=1 || !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cc,"next_message")))
        return fail(e,"invalid saved FILE_COMMIT caption");
    char parent[37],grandparent[37],object[37],version[37];uint64_t base;
    int replace=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(original,"replace"));
    if(!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(original,"replace"))) return fail(e,"invalid replace flag");
    int target=replace?replacement_target(fs,json_string(original,"parent_path"),json_string(original,"name"),object,parent,version,&base,e):
        directory_target(fs,json_string(original,"parent_path"),json_string(original,"name"),parent,grandparent,e);
    if(target || !same(parent,json_string(original,"parent_id")) || (replace && !same(object,json_string(original,"object_id"))))
        return fail(e,"captured upload destination changed");
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),rc=-1;struct stat st;
    unsigned char *bytes=NULL;cJSON *intent=NULL,*parts=NULL,*current=NULL,*body=NULL;size_t length=0;
    if(dir<0) return fail(e,"cannot reopen upload state");
    if(fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {fail(e,"upload state permissions changed");goto done;}
    current=json_load_at(dir,"intent.json",e);
    if(!current || !cJSON_Compare(current,original,1)) {fail(e,"intent changed during upload preflight");goto done;}
    if(fstatat(dir,"data.bin",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        fail(e,"upload staging permissions changed");goto done;
    }
    bytes=bytes_load_at(dir,"data.bin",PUT_MAX_SIZE,&length,e);char hash[65];
    if(!bytes || length!=size || digest(bytes,length,hash) || !same(hash,json_string(original,"file_sha256"))) {fail(e,"upload staging changed");goto done;}
    intent=cJSON_Duplicate(original,1);parts=cJSON_CreateArray();body=cJSON_CreateObject();
    if(!intent || !parts || !body) {fail(e,"out of memory");goto done;}
    /* Verify all reused messages before the first new storage publication. */
    if(check_pin(bot,journal,e)) goto done;
    for(size_t i=0;i<count;i++) if(found[i] && verify_upload_part(bot,journal,found[i],logfd,e)) goto done;
    if(clock_gettime(CLOCK_MONOTONIC,&last_send)) {fail(e,"cannot pace resumed publications");goto done;}
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"parts");
    if(!cJSON_AddItemToObject(intent,"parts",parts)) {fail(e,"out of memory");goto done;}parts=NULL;
    cJSON *refs=cJSON_GetObjectItemCaseSensitive(intent,"parts");
    cJSON *sent=cJSON_AddArrayToObject(report,"uploaded_chunk_ids");
    if(!sent) {fail(e,"out of memory");goto done;}
#ifdef TGFS_TESTING
    send_interval_ms=0;
#endif
    previous=boundary;
    for(size_t i=0;i<count;i++) {
        size_t offset=i*chunk,n=length-offset;if(n>chunk) n=chunk;
        cJSON *part=cJSON_CreateObject(),*caption=envelope("CHUNK",json_string(intent,"object_id"));
        if(!part || !caption || digest(bytes+offset,n,hash) || add_id(part,"offset",(long long)offset) ||
            add_id(part,"part_size",(long long)n) || !cJSON_AddStringToObject(part,"part_sha256",hash) ||
            !cJSON_AddStringToObject(caption,"parent_id",json_string(intent,"parent_id")) ||
            !cJSON_AddStringToObject(caption,"version_id",json_string(intent,"version_id")) ||
            !cJSON_AddNumberToObject(caption,"part_count",(double)count) || add_id(caption,"offset",(long long)offset) ||
            add_id(caption,"part_size",(long long)n) || add_id(caption,"file_size",(long long)size) ||
            !cJSON_AddStringToObject(caption,"part_sha256",hash) || !cJSON_AddStringToObject(caption,"file_sha256",json_string(intent,"file_sha256")) ||
            !cJSON_AddNullToObject(caption,"file_commit_id")) {cJSON_Delete(part);cJSON_Delete(caption);fail(e,"out of memory");goto done;}
        long long id=0;
        if(found[i]) {uint64_t mid;json_decimal(found[i],"message_id",&mid);id=(long long)mid;}
        else {
            char index[32],filename[256];snprintf(index,sizeof(index),"%zu",i);snprintf(filename,sizeof(filename),"%s.%zu",json_string(intent,"name"),i);
            cJSON_DeleteItemFromObjectCaseSensitive(intent,"pending_part");
            cJSON_DeleteItemFromObjectCaseSensitive(intent,"last_attempted_part");
            if(!cJSON_AddNumberToObject(intent,"pending_part",(double)i) ||
                !cJSON_AddNumberToObject(intent,"last_attempted_part",(double)i) || check_pin(bot,journal,e) ||
                upload_request_event(logfd,"upload_chunk_pending",index,json_string(intent,"operation_id"),e) || persist(dir,intent,"chunk_send_pending",e)) {
                cJSON_Delete(part);cJSON_Delete(caption);goto done;
            }
            tgfs_bot_error rejection;
            int uploaded=upload(bot,json_string(journal,"storage_chat_id"),channel,filename,bytes+offset,n,caption,&id,e,&rejection);
            if(uploaded) {
                tgfs_error saved=*e;
                if(!upload_rejection(logfd,"upload_chunk_rejected",index,json_string(intent,"operation_id"),&rejection,e)) *e=saved;
                cJSON_Delete(part);cJSON_Delete(caption);goto done;
            }
            if(id<=(long long)previous) {cJSON_Delete(part);cJSON_Delete(caption);fail(e,"non-increasing resumed CHUNK ID");goto done;}
            previous=(uint64_t)id;
            char text[32];snprintf(text,sizeof(text),"%lld",id);
            if(!cJSON_AddItemToArray(sent,cJSON_CreateString(text))) {cJSON_Delete(part);cJSON_Delete(caption);fail(e,"out of memory");goto done;}
        }
        cJSON_Delete(caption);
        if(add_id(part,"message_id",id) || !cJSON_AddItemToArray(refs,part)) {cJSON_Delete(part);fail(e,"out of memory");goto done;}
        /* Reused prefix is written together with the next pending request or commit. */
        if(!found[i]) {
            cJSON_DeleteItemFromObjectCaseSensitive(intent,"pending_part");
            if(persist(dir,intent,"chunk_sent",e)) goto done;
        }
    }
    if(!cJSON_AddItemToObject(body,"parts",cJSON_Duplicate(refs,1))) {fail(e,"out of memory");goto done;}
    if(commit_retry && !cJSON_Compare(body,cJSON_GetObjectItemCaseSensitive(original,"commit_document"),1)) {
        fail(e,"retry manifest differs from saved FILE_COMMIT");goto done;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"commit_document");
    if(!cJSON_AddItemToObject(intent,"commit_document",cJSON_Duplicate(body,1))) {fail(e,"out of memory");goto done;}
    long long id=0;size_t commit_size;
    if(tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&channel) || check_pin(bot,journal,e) ||
        upload_request_event(logfd,"upload_commit_pending",json_string(intent,"operation_id"),json_string(intent,"operation_id"),e) || persist(dir,intent,"commit_send_pending",e)) goto done;
    tgfs_bot_error rejection;
    int committed=upload_json(bot,json_string(journal,"storage_chat_id"),channel,"file-commit.json",body,cc,&id,&commit_size,e,&rejection);
    if(committed) {
        tgfs_error saved=*e;
        if(!upload_rejection(logfd,"upload_commit_rejected",json_string(intent,"operation_id"),json_string(intent,"operation_id"),&rejection,e)) *e=saved;
        goto done;
    }
    if(id<=(long long)previous) {fail(e,"non-increasing resumed FILE_COMMIT ID");goto done;}
    if(add_id(intent,"file_commit_id",id) || persist(dir,intent,replace?"published_backlinks_pending":"backlinks_pending",e)) goto done;
    char text[32];snprintf(text,sizeof(text),"%lld",id);
    if(upload_log_event(logfd,"upload_commit_confirmed",text,e) ||
        !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(report,"commit_ids"),cJSON_CreateString(text)) ||
        !cJSON_AddStringToObject(report,"upload_resume","committed")) {if(!e->message[0]) fail(e,"cannot record resumed commit");goto done;}
    cJSON_ReplaceItemInObjectCaseSensitive(report,"resumed",cJSON_CreateBool(1));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"scope",cJSON_CreateString("capture_plus_recovery_response"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"local_phase",cJSON_CreateString(replace?"published_backlinks_pending":"backlinks_pending"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"publication",cJSON_CreateString(replace?"confirmed":"not_observed"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"next_step",cJSON_CreateString("capture_after_upload_resume"));
    rc=0;
done:
    free(bytes);cJSON_Delete(current);cJSON_Delete(intent);cJSON_Delete(parts);cJSON_Delete(body);close(dir);return rc;
}
