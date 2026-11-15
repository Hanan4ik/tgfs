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
                  const unsigned char *bytes,size_t size,const cJSON *caption,long long *id,tgfs_error *e)
{
    char *text=cJSON_PrintUnformatted(caption);cJSON *out=NULL;
    if(!text) return fail(e,"out of memory");
    tgfs_bot_error b;
    int rc=pace(e);
    if(!rc) {
        rc=tgfs_bot_send_document(bot,chat,filename,bytes,size,text,&out,&b);
        if(rc) api_fail(e,&b);else rc=check_message(out,channel,filename,size,text,id,e);
    }
    cJSON_Delete(out);free(text);return rc;
}
static int upload_json(tgfs_bot *bot,const char *chat,long long channel,const char *name,
                       const cJSON *body,const cJSON *caption,long long *id,size_t *size,tgfs_error *e)
{
    char *raw=cJSON_PrintUnformatted(body);
    if(!raw) return fail(e,"out of memory");
    *size=strlen(raw);
    int rc=upload(bot,chat,channel,name,(const unsigned char *)raw,*size,caption,id,e);free(raw);return rc;
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

static int put(tgfs_bot *bot,const cJSON *journal,const char *file,const char *name,
               const char *state,size_t chunk_size,tgfs_error *e)
{
    int rc=-1,dir=-1,parts_attached=0;
    unsigned char *bytes=NULL;
    size_t size=0,commit_size=0,directory_size=0;
    cJSON *intent=NULL,*parts=NULL,*captions=NULL,*commit=NULL,*body=NULL,*directory=NULL,*directory_body=NULL;
    const char *chat=json_string(journal,"storage_chat_id");long long channel=0,commit_id=0,directory_id=0;
    const char *root=json_string(cJSON_GetObjectItemCaseSensitive(journal,"superblock"),"root_object_id");
    cJSON *wire=cJSON_CreateString(name);char *encoded=wire?cJSON_PrintUnformatted(wire):NULL;
    cJSON *decoded=encoded?json_parse_bytes((const unsigned char *)encoded,strlen(encoded),e):NULL;
    cJSON_Delete(wire);free(encoded);
    if(!decoded) {fail(e,"filename must be valid UTF-8");goto done;}cJSON_Delete(decoded);
    if(remote_check_root_name(bot,journal,name,e) || tgfs_bot_channel_id(chat,&channel) || !uuid_valid(root) ||
        check_pin(bot,journal,e)) goto done;
    bytes=read_input(file,&size,e);if(!bytes) goto done;
    size_t count=(size+chunk_size-1)/chunk_size;
    if(count>PUT_MAX_PARTS) {fail(e,"too many parts for put prototype");goto done;}
    uint64_t boundary;
    if(json_decimal(journal,"boundary_id",&boundary)) {fail(e,"invalid captured boundary");goto done;}
    long long previous=(long long)boundary;
    char object[37],version[37],operation[37],add[37],full_hash[65];
    if(uuid_new(object) || uuid_new(version) || uuid_new(operation) || uuid_new(add) || digest(bytes,size,full_hash)) {
        fail(e,"cannot generate publication IDs or SHA-256");goto done;
    }
    if(mkdir(state,0700)) {fail(e,"state directory must not exist; preserve previous intent");goto done;}
    dir=open(state,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(dir<0) {fail(e,"cannot open staging directory");goto done;}
    /* Persist the newly created directory entry before any remote mutation. */
    char *parent=strdup(state);
    if(!parent) {fail(e,"out of memory");goto done;}
    size_t parent_length=strlen(parent);
    while(parent_length>1 && parent[parent_length-1]=='/') parent[--parent_length]=0;
    char *slash=strrchr(parent,'/');
    if(slash) {if(slash==parent) slash[1]=0;else *slash=0;}
    else {free(parent);parent=strdup(".");}
    int pfd=parent?open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;free(parent);
    if(pfd<0) {fail(e,"cannot open state parent");goto done;}
    int synced=fsync(pfd);close(pfd);if(synced) {fail(e,"cannot persist state directory");goto done;}
    if(write_file(dir,"data.bin",bytes,size,e)) goto done;
    intent=cJSON_CreateObject();parts=cJSON_CreateArray();captions=cJSON_CreateArray();
    commit=envelope("FILE_COMMIT",object);body=cJSON_CreateObject();
    directory=envelope("DIRECTORY",root);directory_body=cJSON_CreateObject();
    if(!intent || !parts || !captions || !commit || !body || !directory || !directory_body ||
        !cJSON_AddStringToObject(intent,"storage_chat_id",chat) || !cJSON_AddStringToObject(intent,"parent_id",root) ||
        !cJSON_AddStringToObject(intent,"source_boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddStringToObject(intent,"name",name) || !cJSON_AddStringToObject(intent,"object_id",object) ||
        !cJSON_AddStringToObject(intent,"version_id",version) || !cJSON_AddStringToObject(intent,"operation_id",operation) ||
        !cJSON_AddStringToObject(intent,"add_operation_id",add) || !cJSON_AddStringToObject(intent,"file_sha256",full_hash) ||
        add_id(intent,"file_size",(long long)size) || !cJSON_AddNumberToObject(intent,"chunk_bytes",(double)chunk_size) ||
        !cJSON_AddItemToObject(intent,"parts",parts)) {fail(e,"cannot allocate intent");goto done;}
    parts_attached=1;
    if(!cJSON_AddStringToObject(commit,"version_id",version) || !cJSON_AddStringToObject(commit,"operation_id",operation) ||
        !cJSON_AddStringToObject(commit,"parent_id",root) || !cJSON_AddStringToObject(commit,"file_sha256",full_hash) ||
        !cJSON_AddNumberToObject(commit,"part_count",(double)count) || add_id(commit,"file_size",(long long)size) ||
        !cJSON_AddNullToObject(directory,"parent_id")) {fail(e,"out of memory");goto done;}
    cJSON *ops=cJSON_CreateArray(),*entry=cJSON_CreateObject();
    if(!ops || !entry || !cJSON_AddStringToObject(entry,"operation_id",add) || !cJSON_AddStringToObject(entry,"action","ADD") ||
        !cJSON_AddStringToObject(entry,"name",name) || !cJSON_AddStringToObject(entry,"object_id",object) ||
        !cJSON_AddItemToArray(ops,entry)) {cJSON_Delete(ops);cJSON_Delete(entry);fail(e,"out of memory");goto done;}
    if(!cJSON_AddItemToObject(directory_body,"operations",ops)) {cJSON_Delete(ops);fail(e,"out of memory");goto done;}
    if(!cJSON_AddItemToObject(intent,"directory_document",cJSON_Duplicate(directory_body,1)) ||
        !cJSON_AddItemToObject(intent,"commit_caption",cJSON_Duplicate(commit,1))) {fail(e,"out of memory");goto done;}
    if(persist(dir,intent,"prepared",e)) goto done;
    for(size_t i=0;i<count;i++) {
        size_t offset=i*chunk_size,length=size-offset;if(length>chunk_size) length=chunk_size;
        char hash[65],filename[256];snprintf(filename,sizeof(filename),"%s.%zu",name,i);
        cJSON *caption=envelope("CHUNK",object),*part=cJSON_CreateObject();
        if(digest(bytes+offset,length,hash) || !caption || !part ||
            !cJSON_AddStringToObject(caption,"parent_id",root) || !cJSON_AddStringToObject(caption,"version_id",version) ||
            !cJSON_AddNumberToObject(caption,"part_count",(double)count) || add_id(caption,"offset",(long long)offset) ||
            add_id(caption,"part_size",(long long)length) || add_id(caption,"file_size",(long long)size) ||
            !cJSON_AddStringToObject(caption,"part_sha256",hash) || !cJSON_AddStringToObject(caption,"file_sha256",full_hash) ||
            !cJSON_AddNullToObject(caption,"file_commit_id") ||
            add_id(part,"offset",(long long)offset) || add_id(part,"part_size",(long long)length) ||
            !cJSON_AddStringToObject(part,"part_sha256",hash)) {
            cJSON_Delete(caption);cJSON_Delete(part);fail(e,"out of memory");goto done;
        }
        if(!cJSON_AddItemToArray(captions,caption)) {cJSON_Delete(caption);cJSON_Delete(part);fail(e,"out of memory");goto done;}
        if(!cJSON_AddNumberToObject(intent,"pending_part",(double)i) || persist(dir,intent,"chunk_send_pending",e)) {
            cJSON_Delete(part);goto done;
        }
        long long id=0;
        int uploaded=upload(bot,chat,channel,filename,bytes+offset,length,caption,&id,e);
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
    if(!cJSON_AddItemToObject(body,"parts",cJSON_Duplicate(parts,1)) ||
        !cJSON_AddItemToObject(intent,"commit_document",cJSON_Duplicate(body,1)) || persist(dir,intent,"commit_send_pending",e)) goto done;
    int committed=upload_json(bot,chat,channel,"file-commit.json",body,commit,&commit_id,&commit_size,e);
    if(!committed && commit_id<=previous) committed=fail(e,"non-increasing commit ID");
    if(commit_id>0 && add_id(intent,"file_commit_id",commit_id)) {fail(e,"out of memory");goto done;}
    if(committed) {tgfs_error saved=*e;persist(dir,intent,"commit_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,"backlinks_pending",e)) goto done;
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
    if(check_pin(bot,journal,e) || persist(dir,intent,"directory_send_pending",e)) goto done;
    int published=upload_json(bot,chat,channel,"directory.json",directory_body,directory,&directory_id,&directory_size,e);
    (void)directory_size;
    if(!published && directory_id<=commit_id) published=fail(e,"non-increasing directory ID");
    if(directory_id>0 && add_id(intent,"directory_message_id",directory_id)) {fail(e,"out of memory");goto done;}
    if(published) {tgfs_error saved=*e;persist(dir,intent,"directory_response_uncertain",e);*e=saved;goto done;}
    if(persist(dir,intent,"published_navigation_pending",e)) goto done;
    cJSON_DeleteItemFromObjectCaseSensitive(commit,"next_message");
    if(add_id(commit,"next_message",directory_id) || edit(bot,chat,channel,commit_id,"file-commit.json",commit_size,commit,e)) goto done;
    if(persist(dir,intent,"complete",e)) goto done;
    printf("OK put name=%s object_id=%s parts=%zu bytes=%zu commit_id=%lld directory_id=%lld\n",
        name,object,count,size,commit_id,directory_id);rc=0;
done:
    if(rc && intent) {
        const cJSON *last=cJSON_GetArrayItem(parts,cJSON_GetArraySize(parts)-1);
        fprintf(stderr,"put state: phase=%s commit_id=%lld directory_id=%lld last_chunk_id=%s\n",
            json_string(intent,"phase")?json_string(intent,"phase"):"preparing",commit_id,directory_id,
            json_string(last,"message_id")?json_string(last,"message_id"):"unknown");
    }
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
          "Publish a NEW file in the captured root: CHUNK -> FILE_COMMIT -> DIRECTORY ADD.\n"
          "  --capture DIRECTORY      Completed journal; root pin must still match.\n"
          "  --file PATH              Stable regular input file, max 64 MiB; empty is allowed.\n"
          "  --name NAME              New root filename, max 245 bytes (no path separators).\n"
          "  --state-dir DIRECTORY    New private staging/intent directory; preserve on failure.\n"
          "  --token-file PATH        Owner-only token file for the bot used by scan.\n"
          "  --chunk-bytes N          1..20971520, default 20971520; max 4096 parts.\n"
          "  --proxy URL | --direct   Same proxy settings as read.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --send-interval-ms N     Minimum mutation interval, 0..10000, default 1100.\n"
          "  -h, --help               Show help without network.\n"
          "No overwrite, automatic resume or retry. Concurrent same-name creates may conflict.\n",stream);
}
int tgfs_put_main(int argc,char **argv)
{
    const char *capture=NULL,*file=NULL,*name=NULL,*state=NULL,*token=NULL,*proxy=NULL,*timeout_text=NULL,*chunk_text=NULL,*interval_text=NULL;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_put_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
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
    if(!capture || !file || !name_valid(name) || strchr(name,'\r') || strchr(name,'\n') || strlen(name)>245 || !state || !*state || !token ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300)) ||
        (chunk_text && (decimal(chunk_text,&chunk) || !chunk || chunk>TGFS_MAX_CHUNK)) ||
        (interval_text && (decimal(interval_text,&interval) || interval>10000))) goto usage;
    send_interval_ms=(unsigned)interval;last_send=(struct timespec){0};
    tgfs_error e={0};tgfs_bot_error b;tgfs_bot *bot=NULL;cJSON *journal=NULL;int rc=1;
    int dir=open(capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {fail(&e,"cannot open capture");goto done;}
    journal=json_load_at(dir,"journal.json",&e);close(dir);if(!journal) goto done;
    if(tgfs_bot_open(token,(unsigned)timeout,&bot,&b) || tgfs_bot_set_proxy(bot,proxy,&b)) {api_fail(&e,&b);goto done;}
    rc=put(bot,journal,file,name,state,(size_t)chunk,&e) ? 1 : 0;
done:
    if(rc) fprintf(stderr,"FAIL put: %s; preserve state directory\n",e.message);
    cJSON_Delete(journal);tgfs_bot_close(bot);return rc;
usage:
    tgfs_put_help(stderr);return 2;
}
