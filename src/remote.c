#include "tgfs/scan.h"
#include "tgfs/bot_api.h"
#include "internal.h"
#include "cache.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define META_BUDGET (64U * 1024U * 1024U)
typedef struct {
    tgfs_bot *bot;
    const cJSON *records;
    tgfs_cache *cache;
    int offline,refresh,index_hit;
    unsigned hits,downloads;
    char scope[65],capture_key[65];
} remote;
static int same(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static int api_failure(tgfs_error *e,const tgfs_bot_error *b)
{
    return fail(e,"%s [http=%ld api=%d retry_after=%u]",b->message,b->http_status,b->api_code,b->retry_after);
}
static unsigned char *network_download(remote *r,const cJSON *record,size_t limit,size_t *length,tgfs_error *e)
{
    const cJSON *doc=cJSON_GetObjectItemCaseSensitive(record,"document");
    const char *fid=json_string(doc,"file_id");
    long long expected,actual;
    if(!fid || !*fid || tgfs_bot_integer(doc,"file_size",&expected) || expected<0 || (uint64_t)expected>limit) {
        fail(e,"invalid captured document metadata or size limit");return NULL;
    }
    cJSON *p=cJSON_CreateObject(),*out=NULL;
    if(!p || !cJSON_AddStringToObject(p,"file_id",fid)) {cJSON_Delete(p);fail(e,"out of memory");return NULL;}
    tgfs_bot_error b;
    int rc=tgfs_bot_call(r->bot,"getFile",p,&out,&b);cJSON_Delete(p);
    if(rc) {api_failure(e,&b);return NULL;}
    const char *path=json_string(out,"file_path");
    if(!path || !same(json_string(out,"file_id"),fid) ||
        tgfs_bot_integer(out,"file_size",&actual) || actual!=expected) {
        cJSON_Delete(out);fail(e,"getFile metadata disagrees with capture");return NULL;
    }
    unsigned char *bytes=NULL;
    rc=tgfs_bot_download(r->bot,path,&bytes,length,&b);cJSON_Delete(out);
    if(rc) {api_failure(e,&b);return NULL;}
    if(*length!=(size_t)expected || *length>limit) {free(bytes);fail(e,"download size disagrees with capture");return NULL;}
    if(!bytes) {bytes=malloc(1);if(!bytes) fail(e,"out of memory");}
    return bytes;
}
static int scope_keys(remote *r,const cJSON *journal,tgfs_error *e)
{
    if(!r->cache) return 0;
    long long bot_id;
    if(tgfs_bot_id(r->bot,&bot_id)) return fail(e,"cannot identify bot cache scope");
    const cJSON *sb=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    const char *root=json_string(sb,"root_object_id");
    if(!uuid_valid(root)) return fail(e,"invalid root cache scope");
    cJSON *key=cJSON_CreateObject();char captured[65];
    if(!key || !cJSON_AddNumberToObject(key,"layout",1) || !cJSON_AddNumberToObject(key,"bot_id",(double)bot_id) ||
        !cJSON_AddStringToObject(key,"channel",json_string(journal,"storage_chat_id")) ||
        !cJSON_AddStringToObject(key,"root",root) || !cJSON_AddStringToObject(key,"start_id",json_string(sb,"start_id"))) {
        cJSON_Delete(key);return fail(e,"out of memory");
    }
    int rc=cache_json_key(key,r->scope,e);cJSON_Delete(key);
    if(rc || cache_json_key(journal,captured,e)) return -1;
    char text[160];snprintf(text,sizeof(text),"index_v1:%s:%s",r->scope,captured);
    return cache_hash(text,strlen(text),r->capture_key,e);
}
static int document_key(remote *r,const cJSON *record,char key[65],tgfs_error *e)
{
    const cJSON *doc=cJSON_GetObjectItemCaseSensitive(record,"document");
    const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
    cJSON *value=cJSON_CreateObject();long long size;
    const char *fid=json_string(doc,"file_id"),*type=json_string(caption,"type");
    if(!fid || !*fid || !type || tgfs_bot_integer(doc,"file_size",&size) || size<0) {
        cJSON_Delete(value);return fail(e,"invalid captured document metadata");
    }
    if(!value || !cJSON_AddStringToObject(value,"scope",r->scope) ||
        !cJSON_AddStringToObject(value,"source_id",json_string(record,"message_id")) ||
        !cJSON_AddStringToObject(value,"type",type) || !cJSON_AddStringToObject(value,"file_id",fid) ||
        !cJSON_AddNumberToObject(value,"size",(double)size) ||
        (same(type,"CHUNK") && !cJSON_AddStringToObject(value,"sha256",json_string(caption,"part_sha256")))) {
        cJSON_Delete(value);return fail(e,"invalid document identity or out of memory");
    }
    int rc=cache_json_key(value,key,e);cJSON_Delete(value);return rc;
}
static int chunk_digest(const unsigned char *bytes,size_t length,const cJSON *record,tgfs_error *e)
{
    const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
    const char *expected=json_string(caption,"part_sha256");char hash[65];
    return hash_valid(expected) && !cache_hash(bytes,length,hash,e) && same(hash,expected);
}
static unsigned char *download(remote *r,const cJSON *record,size_t limit,size_t *length,tgfs_error *e)
{
    char key[65];unsigned char *bytes=NULL;
    const cJSON *doc=cJSON_GetObjectItemCaseSensitive(record,"document");
    long long expected;
    const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
    int chunk=same(json_string(caption,"type"),"CHUNK");
    if(tgfs_bot_integer(doc,"file_size",&expected) || expected<0 || (uint64_t)expected>limit) {
        fail(e,"invalid captured document metadata or size limit");return NULL;
    }
    if(r->cache) {
        if(document_key(r,record,key,e)) return NULL;
        if(!r->refresh) {
            int hit=cache_blob_get(r->cache,key,limit,&bytes,length,e);
            if(hit<0) return NULL;
            if(hit && (*length!=(size_t)expected || (chunk && !chunk_digest(bytes,*length,record,e)))) {
                free(bytes);bytes=NULL;
                if(cache_blob_drop(r->cache,key,e)) return NULL;
                hit=0;
            }
            if(hit) {r->hits++;return bytes;}
        }
    }
    if(r->offline) {fail(e,"offline cache miss for source message %s",json_string(record,"message_id"));return NULL;}
    bytes=network_download(r,record,limit,length,e);
    if(!bytes) return NULL;
    r->downloads++;
    if(chunk) {
        if(!chunk_digest(bytes,*length,record,e)) {free(bytes);fail(e,"downloaded CHUNK size or SHA-256 mismatch");return NULL;}
    } else {
        cJSON *parsed=json_parse_bytes(bytes,*length,e);
        if(!parsed) {free(bytes);return NULL;}
        int object=cJSON_IsObject(parsed);cJSON_Delete(parsed);
        if(!object) {free(bytes);fail(e,"metadata document must be a JSON object");return NULL;}
    }
    if(r->cache && cache_blob_put(r->cache,key,bytes,*length,e)) {free(bytes);return NULL;}
    return bytes;
}
static unsigned char *load_chunk(uint64_t id,size_t *length,void *context,tgfs_error *e)
{
    remote *r=context;
    const cJSON *record;
    cJSON_ArrayForEach(record,r->records) {
        uint64_t mid;
        if(!json_decimal(record,"message_id",&mid) && mid==id) {
            const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
            if(!same(json_string(caption,"type"),"CHUNK")) break;
            return download(r,record,TGFS_MAX_CHUNK,length,e);
        }
    }
    fail(e,"selected CHUNK is missing from journal");return NULL;
}
static int write_file(int dir,const char *name,const unsigned char *bytes,size_t length,tgfs_error *e)
{
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return fail(e,"cannot create temporary metadata file");
    size_t done=0;int rc=0;
    while(done<length) {
        ssize_t n=write(fd,bytes+done,length-done);
        if(n<=0) {rc=fail(e,"cannot write temporary metadata");break;}
        done+=(size_t)n;
    }
    if(close(fd)) rc=fail(e,"cannot close temporary metadata");
    return rc;
}
static int emit(const char *name,const char *object,void *arg)
{
    (void)object;return fprintf(arg,"%s\n",name)<0;
}
static void clean(int dir,const char *path)
{
    if(dir<0) return;
    DIR *stream=fdopendir(dup(dir));
    if(stream) {
        struct dirent *entry;
        while((entry=readdir(stream)))
            if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) unlinkat(dir,entry->d_name,0);
        closedir(stream);
    }
    close(dir);rmdir(path);
}
typedef struct { const char *name; int found; } name_check;
static int check_entry(const char *name,const char *object,void *arg)
{
    (void)object;name_check *check=arg;
    if(same(name,check->name)) check->found=1;
    return 0;
}
static int read_capture(remote *r,const cJSON *journal,const char *command,const char *path,const char *new_name,tgfs_error *e)
{
    const cJSON *sb=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    uint64_t start,boundary;
    const char *storage=json_string(journal,"storage_chat_id");
    long long channel;
    r->records=cJSON_GetObjectItemCaseSensitive(journal,"records");
    if(!same(json_string(journal,"schema"),"TGFS_JOURNAL_CAPTURE_V1") ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(journal,"scan_completed")) ||
        !same(json_string(journal,"gap_policy"),"forward_400_is_gap") ||
        tgfs_bot_channel_id(storage,&channel) || json_decimal(sb,"start_id",&start) || !start ||
        json_decimal(journal,"boundary_id",&boundary) || boundary<=start || boundary>4503599627370495ULL ||
        !cJSON_IsArray(r->records) || cJSON_GetArraySize(r->records)>TGFS_MAX_RECORDS)
        return fail(e,"invalid or incomplete journal capture");
    if(scope_keys(r,journal,e)) return -1;
    if(!r->offline) {
    cJSON *p=cJSON_CreateObject(),*chat=NULL;
    if(!p || !cJSON_AddStringToObject(p,"chat_id",storage)) {cJSON_Delete(p);return fail(e,"out of memory");}
    tgfs_bot_error b;
    int rc=tgfs_bot_call(r->bot,"getChat",p,&chat,&b);cJSON_Delete(p);
    if(rc) return api_failure(e,&b);
    long long received;
    int valid=!tgfs_bot_integer(chat,"id",&received) && received==channel && same(json_string(chat,"type"),"channel");
    cJSON_Delete(chat);
    if(!valid) return fail(e,"captured channel identity mismatch");
    }
    char temp[]="/tmp/tgfs-read-XXXXXX";
    if(!mkdtemp(temp)) return fail(e,"cannot create private metadata workspace");
    int dir=open(temp,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {rmdir(temp);return fail(e,"cannot open metadata workspace");}
    cJSON *snapshot=NULL,*messages=NULL;
    tgfs *fs=NULL;
    int rc=-1;
    if(r->cache && !r->refresh) {
        int hit=cache_index_get(r->cache,r->capture_key,&snapshot,e);
        if(hit<0) goto done;
        r->index_hit=hit;
    }
    if(!snapshot) {
        snapshot=cJSON_CreateObject();messages=cJSON_CreateArray();
        if(!snapshot || !messages) {fail(e,"out of memory");goto done;}
        if(!cJSON_AddItemToObject(snapshot,"superblock",cJSON_Duplicate(sb,1)) ||
            !cJSON_AddItemToObject(snapshot,"messages",messages)) {fail(e,"out of memory");goto done;}
        messages=NULL;
    }
    size_t total=0;
    const cJSON *record;
    uint64_t previous=start;
    cJSON_ArrayForEach(record,r->records) {
        uint64_t mid;
        const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
        size_t version;
        if(json_decimal(record,"message_id",&mid) || mid<=previous || mid>=boundary ||
            !same(json_string(caption,"magic"),"TGFS") || json_count(caption,"format_version",&version) || version!=1) {
            fail(e,"invalid, duplicate or unsorted journal record");goto done;
        }
        previous=mid;
        const char *type=json_string(caption,"type");
        if(same(type,"SCAN_MARKER")) continue;
        if(!same(type,"DIRECTORY") && !same(type,"FILE_COMMIT") && !same(type,"CHUNK")) {
            fail(e,"unsupported TGFS journal record type");goto done;
        }
        char name[48];snprintf(name,sizeof(name),"%" PRIu64 ".json",mid);
        cJSON *entry=r->index_hit?NULL:cJSON_CreateObject();
        if(!r->index_hit && (!entry || !cJSON_AddStringToObject(entry,"message_id",json_string(record,"message_id")) ||
            !cJSON_AddStringToObject(entry,"document",name) ||
            !cJSON_AddItemToObject(entry,"caption",cJSON_Duplicate(caption,1)))) {
            cJSON_Delete(entry);fail(e,"out of memory");goto done;
        }
        if(same(type,"CHUNK")) {
            const char *filename=json_string(cJSON_GetObjectItemCaseSensitive(record,"document"),"file_name");
            if(!name_valid(filename) || (!r->index_hit && !cJSON_AddStringToObject(entry,"filename",filename))) {
                cJSON_Delete(entry);fail(e,"invalid captured CHUNK filename");goto done;
            }
        } else {
            size_t length=0;
            unsigned char *bytes=download(r,record,TGFS_MAX_JSON,&length,e);
            if(!bytes) {cJSON_Delete(entry);goto done;}
            if(length>META_BUDGET-total) {free(bytes);cJSON_Delete(entry);fail(e,"metadata budget exceeded");goto done;}
            total+=length;
            int written=write_file(dir,name,bytes,length,e);free(bytes);
            if(written) {cJSON_Delete(entry);goto done;}
        }
        if(!r->index_hit && !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(snapshot,"messages"),entry)) {
            cJSON_Delete(entry);fail(e,"out of memory");goto done;
        }
    }
    char *raw=cJSON_PrintUnformatted(snapshot);
    if(!raw) {fail(e,"out of memory");goto done;}
    size_t length=strlen(raw);
    int written=length>TGFS_MAX_JSON ? fail(e,"snapshot metadata exceeds limit") :
        write_file(dir,"snapshot.json",(const unsigned char *)raw,length,e);
    free(raw);
    if(written || tgfs_open_snapshot_loader(temp,load_chunk,r,&fs,e)) goto done;
    if(r->cache && !r->index_hit && cache_index_put(r->cache,r->capture_key,snapshot,journal,e)) goto done;
    if(new_name) {
        name_check check={.name=new_name};
        rc=tgfs_list(fs,"/",check_entry,&check,e);
        if(!rc && check.found) rc=fail(e,"name already exists in captured root");
    } else rc=same(command,"ls") ? tgfs_list(fs,path,emit,stdout,e) : tgfs_cat(fs,path,stdout,e);
    if(!new_name && !rc && fflush(stdout)) rc=fail(e,"cannot flush output");
done:
    tgfs_close(fs);cJSON_Delete(snapshot);cJSON_Delete(messages);clean(dir,temp);return rc;
}
int remote_check_root_name(tgfs_bot *bot,const cJSON *journal,const char *name,tgfs_error *e)
{
    remote r={.bot=bot};
    return read_capture(&r,journal,"ls","/",name,e);
}
void tgfs_read_help(FILE *stream)
{
    fputs("usage: tgfs read --capture DIRECTORY --token-file PATH [options] {ls|cat} /absolute/path\n"
          "Read a captured Telegram journal; download DIRECTORY/FILE_COMMIT metadata,\n"
          "merge directory operations, fetch only selected CHUNKs for cat, verify SHA-256.\n"
          "  --capture DIRECTORY      Completed scan containing journal.json.\n"
          "  --token-file PATH        Owner-only token file for the same bot as scan.\n"
          "  --proxy URL | --direct   Same routing options as probe.\n"
          "  --timeout-seconds N      1..300 seconds, default 60.\n"
          "  --cache-dir DIRECTORY    Private persistent SQLite cache (default XDG_CACHE_HOME/tgfs).\n"
          "  --cache-limit-mib N      Blob budget 1..4096 MiB, default 256.\n"
          "  --offline                Cached data only; no Telegram requests.\n"
          "  --refresh-cache          Fetch documents again and rebuild this index.\n"
          "  --no-cache               Use temporary metadata without persistent cache.\n"
          "  --cache-stats            Print index/hit/download counters to stderr.\n"
          "  -h, --help               Show help without network.\n"
          "Uses the captured boundary; does not scan, publish or mount a channel.\n",stream);
}
int tgfs_read_main(int argc,char **argv)
{
    const char *capture=NULL,*token=NULL,*proxy=NULL,*timeout_text=NULL,*cache_dir=NULL,*limit_text=NULL;
    int no_cache=0,offline=0,refresh=0,stats=0;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_read_help(stdout);return 0;}
    if(argc<4 || (!same(argv[argc-2],"ls") && !same(argv[argc-2],"cat"))) goto usage;
    for(int i=2;i<argc-2;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--no-cache")) {if(no_cache) goto usage;no_cache=1;continue;}
        if(same(argv[i],"--offline")) {if(offline) goto usage;offline=1;continue;}
        if(same(argv[i],"--refresh-cache")) {if(refresh) goto usage;refresh=1;continue;}
        if(same(argv[i],"--cache-stats")) {if(stats) goto usage;stats=1;continue;}
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--capture")) slot=&capture;
        else if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        else if(same(argv[i],"--cache-dir")) slot=&cache_dir;
        else if(same(argv[i],"--cache-limit-mib")) slot=&limit_text;
        if(!slot || *slot || i+1>=argc-2) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60,limit_mib=256;
    if((no_cache && (offline || refresh || cache_dir || limit_text)) || (offline && refresh) ||
        (limit_text && (decimal(limit_text,&limit_mib) || !limit_mib || limit_mib>4096 || limit_mib>SIZE_MAX/(1024U*1024U)))) goto usage;
    if(!capture || !token || argv[argc-1][0]!='/' ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300))) goto usage;
    tgfs_error e={{0}};tgfs_bot_error b;
    remote r={.offline=offline,.refresh=refresh};cJSON *journal=NULL;
    int dir=open(capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),rc=1;
    if(dir<0) {fail(&e,"cannot open capture directory");goto done;}
    journal=json_load_at(dir,"journal.json",&e);close(dir);
    if(!journal) goto done;
    if(tgfs_bot_open(token,(unsigned)timeout,&r.bot,&b) || tgfs_bot_set_proxy(r.bot,proxy,&b)) {
        api_failure(&e,&b);goto done;
    }
    if(!no_cache && cache_open(cache_dir,(size_t)limit_mib*1024U*1024U,&r.cache,&e)) goto done;
    rc=read_capture(&r,journal,argv[argc-2],argv[argc-1],NULL,&e) ? 1 : 0;
done:
    if(rc) fprintf(stderr,"FAIL read: %s\n",e.message);
    if(stats) fprintf(stderr,"cache: index=%s hits=%u downloads=%u\n",r.index_hit?"hit":"miss",r.hits,r.downloads);
    cache_close(r.cache);tgfs_bot_close(r.bot);cJSON_Delete(journal);return rc;
usage:
    tgfs_read_help(stderr);return 2;
}
