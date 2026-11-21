#include "tgfs/scan.h"
#include "tgfs/bot_api.h"
#include "internal.h"
#include "cache.h"
#include "tgfs/remote.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>

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
static int read_capture(remote *r,const cJSON *journal,const char *command,const char *path,tgfs **opened,tgfs_error *e)
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
    if(opened) {*opened=fs;fs=NULL;rc=0;goto done;}
    rc=same(command,"ls") ? tgfs_list(fs,path,emit,stdout,e) : tgfs_cat(fs,path,stdout,e);
    if(!rc && fflush(stdout)) rc=fail(e,"cannot flush output");
done:
    tgfs_close(fs);cJSON_Delete(snapshot);cJSON_Delete(messages);clean(dir,temp);return rc;
}
struct tgfs_remote_session { remote r; cJSON *journal; tgfs *fs; };
int tgfs_remote_open(const tgfs_remote_options *o,tgfs_remote_session **out,tgfs_error *e)
{
    if(!out) return fail_code(e,EINVAL,"missing remote session output");
    *out=NULL;
    if(!o || !o->capture || !o->token_file || (o->offline && (o->no_cache || o->refresh)))
        return fail_code(e,EINVAL,"invalid remote session options");
    tgfs_remote_session *s=calloc(1,sizeof(*s));
    if(!s) return fail_code(e,ENOMEM,"out of memory");
    s->r.offline=o->offline;s->r.refresh=o->refresh;
    int dir=open(o->capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {fail(e,"cannot open capture directory");goto invalid;}
    s->journal=json_load_at(dir,"journal.json",e);close(dir);
    if(!s->journal) goto invalid;
    tgfs_bot_error b;
    if(tgfs_bot_open(o->token_file,o->timeout,&s->r.bot,&b) || tgfs_bot_set_proxy(s->r.bot,o->proxy,&b)) {
        api_failure(e,&b);goto invalid;
    }
    if(!o->no_cache && cache_open(o->cache_dir,o->cache_limit,&s->r.cache,e)) goto invalid;
    if(read_capture(&s->r,s->journal,NULL,NULL,&s->fs,e)) goto invalid;
    *out=s;return 0;
invalid:
    tgfs_remote_close(s);return -1;
}
tgfs *tgfs_remote_fs(tgfs_remote_session *s) {return s->fs;}
const cJSON *tgfs_remote_journal(const tgfs_remote_session *s) {return s->journal;}
void tgfs_remote_report(tgfs_remote_session *s,FILE *stream)
{
    fprintf(stream,"cache: index=%s hits=%u downloads=%u\n",s->r.index_hit?"hit":"miss",s->r.hits,s->r.downloads);
}
void tgfs_remote_close(tgfs_remote_session *s)
{
    if(s) {tgfs_close(s->fs);cache_close(s->r.cache);tgfs_bot_close(s->r.bot);cJSON_Delete(s->journal);free(s);}
}
int remote_directory_target(tgfs_bot *bot,const cJSON *journal,const char *path,const char *name,
                            char object[37],char parent[37],tgfs_error *e)
{
    remote r={.bot=bot};tgfs *fs=NULL;
    int rc=read_capture(&r,journal,NULL,NULL,&fs,e);
    if(!rc) rc=directory_target(fs,path,name,object,parent,e);
    tgfs_close(fs);return rc;
}
int remote_replacement_target(tgfs_bot *bot,const cJSON *journal,const char *path,const char *name,
                              char object[37],char parent[37],char version[37],uint64_t *commit_id,tgfs_error *e)
{
    remote r={.bot=bot};tgfs *fs=NULL;
    int rc=read_capture(&r,journal,NULL,NULL,&fs,e);
    if(!rc) rc=replacement_target(fs,path,name,object,parent,version,commit_id,e);
    tgfs_close(fs);return rc;
}
int remote_unlink_target(tgfs_bot *bot,const cJSON *journal,const char *path,const char *name,
                         char object[37],char parent[37],char grandparent[37],cJSON **observed,tgfs_error *e)
{
    remote r={.bot=bot};tgfs *fs=NULL;*observed=NULL;
    int rc=read_capture(&r,journal,NULL,NULL,&fs,e);
    if(!rc) rc=unlink_target(fs,path,name,object,parent,grandparent,observed,e);
    tgfs_close(fs);return rc;
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
    tgfs_error e={0};tgfs_bot_error b;
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

static int intent_stage(remote *r,const cJSON *journal,const cJSON *caption,const cJSON *body,
                        const char *operation,cJSON *matches,tgfs_error *e)
{
    if(!caption || !body) return 0;
    const cJSON *record;int count=0;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *c=cJSON_GetObjectItemCaseSensitive(record,"record");
        if(!same(json_string(c,"type"),json_string(caption,"type")) ||
            !same(json_string(c,"object_id"),json_string(caption,"object_id"))) continue;
        if(same(json_string(c,"type"),"FILE_COMMIT") && !same(json_string(c,"operation_id"),operation)) continue;
        size_t length=0;
        unsigned char *bytes=network_download(r,record,TGFS_MAX_JSON,&length,e);
        if(!bytes) return -1;
        cJSON *document=json_parse_bytes(bytes,length,e);free(bytes);if(!document) return -1;
        int relevant=!operation || same(json_string(c,"type"),"FILE_COMMIT");
        const cJSON *op;
        if(!relevant) cJSON_ArrayForEach(op,cJSON_GetObjectItemCaseSensitive(document,"operations"))
            if(same(json_string(op,"operation_id"),operation)) relevant=1;
        int equal=cJSON_Compare(document,body,1);cJSON_Delete(document);
        if(!relevant) continue;
        if(!equal && !operation) continue; /* Other publications of the same directory are expected. */
        if(!equal || !json_semantic_equal(c,caption)) return fail(e,"intent publication disagrees with captured contents");
        cJSON *id=cJSON_CreateString(json_string(record,"message_id"));
        if(!id || !cJSON_AddItemToArray(matches,id)) {cJSON_Delete(id);return fail(e,"out of memory");}
        count++;
    }
    return count;
}
static int report_id(cJSON *array,const char *id,tgfs_error *e)
{
    cJSON *v=cJSON_CreateString(id);
    if(!v || !cJSON_AddItemToArray(array,v)) {cJSON_Delete(v);return fail(e,"out of memory");}
    return 0;
}
static int intent_data_plan(remote *r,const char *state,const cJSON *intent,const cJSON *journal,cJSON *report,tgfs_error *e)
{
    cJSON *verified=cJSON_AddArrayToObject(report,"verified_chunk_ids");
    cJSON *missing=cJSON_AddArrayToObject(report,"unobserved_part_indexes");
    cJSON *ambiguous=cJSON_AddArrayToObject(report,"ambiguous_part_indexes");
    cJSON *unknown=cJSON_AddArrayToObject(report,"unobserved_known_ids");
    cJSON *repairs=cJSON_AddArrayToObject(report,"backlink_repairs");
    if(!verified || !missing || !ambiguous || !unknown || !repairs || !cJSON_AddBoolToObject(report,"can_resume",0)) return fail(e,"out of memory");
    if(!same(json_string(intent,"kind"),"put")) {
        if(!cJSON_AddStringToObject(report,"staging","not_applicable") ||
            !cJSON_AddStringToObject(report,"next_step","metadata_review_only")) return fail(e,"out of memory");
        return 0;
    }
    uint64_t size,chunk;
    const cJSON *chunk_value=cJSON_GetObjectItemCaseSensitive(intent,"chunk_bytes");
    if(json_decimal(intent,"file_size",&size) || size>64U*1024U*1024U || !cJSON_IsNumber(chunk_value) ||
        chunk_value->valuedouble<1 || chunk_value->valuedouble>TGFS_MAX_CHUNK ||
        chunk_value->valuedouble!=(double)(uint64_t)chunk_value->valuedouble ||
        !uuid_valid(json_string(intent,"version_id")) || !hash_valid(json_string(intent,"file_sha256")) ||
        !name_valid(json_string(intent,"name"))) return fail(e,"invalid put staging metadata");
    chunk=(uint64_t)chunk_value->valuedouble;
    size_t count=(size_t)((size+chunk-1)/chunk);
    if(count>4096 || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(intent,"parts")) ||
        (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(intent,"parts"))>count) return fail(e,"invalid staging parts");
    const cJSON *cc=cJSON_GetObjectItemCaseSensitive(intent,"commit_caption");size_t declared;
    if(!same(json_string(cc,"file_size"),json_string(intent,"file_size")) ||
        !same(json_string(cc,"file_sha256"),json_string(intent,"file_sha256")) ||
        !same(json_string(cc,"object_id"),json_string(intent,"object_id")) ||
        !same(json_string(cc,"parent_id"),json_string(intent,"parent_id")) ||
        !same(json_string(cc,"version_id"),json_string(intent,"version_id")) ||
        json_count(cc,"part_count",&declared) || declared!=count) return fail(e,"intent commit disagrees with staging metadata");
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(dir<0) return fail(e,"cannot reopen staging directory");
    if(fstatat(dir,"data.bin",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        close(dir);return fail(e,"data.bin must be an owner-only regular file");
    }
    size_t length=0;unsigned char *data=bytes_load_at(dir,"data.bin",64U*1024U*1024U,&length,e);close(dir);
    if(!data) return -1;
    int rc=-1;char hash[65];unsigned *seen=calloc(count?count:1,sizeof(*seen));
    if(!seen) {fail(e,"out of memory");goto done;}
    if(length!=size || cache_hash(data,length,hash,e) || !same(hash,json_string(intent,"file_sha256"))) {fail(e,"staging size or SHA-256 mismatch");goto done;}
    const cJSON *commit_ids=cJSON_GetObjectItemCaseSensitive(report,"commit_ids");
    const cJSON *first=cJSON_GetArrayItem(commit_ids,0);const char *canonical=cJSON_IsString(first)?first->valuestring:NULL;
    const cJSON *parts=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(intent,"commit_document"),"parts");
    const cJSON *record;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *c=cJSON_GetObjectItemCaseSensitive(record,"record");
        if(!same(json_string(c,"type"),"CHUNK") || !same(json_string(c,"object_id"),json_string(intent,"object_id")) ||
            !same(json_string(c,"version_id"),json_string(intent,"version_id"))) continue;
        uint64_t offset,part_size;size_t part_count;
        if(json_decimal(c,"offset",&offset) || json_decimal(c,"part_size",&part_size) || offset>=size || offset%chunk ||
            json_count(c,"part_count",&part_count) || part_count!=count || part_size!=(size-offset<chunk?size-offset:chunk) ||
            !same(json_string(c,"parent_id"),json_string(intent,"parent_id")) ||
            !same(json_string(c,"file_sha256"),json_string(intent,"file_sha256")) ||
            !same(json_string(c,"file_size"),json_string(intent,"file_size"))) {fail(e,"captured CHUNK disagrees with staging metadata");goto done;}
        size_t index=(size_t)(offset/chunk);char filename[300];snprintf(filename,sizeof(filename),"%s.%zu",json_string(intent,"name"),index);
        if(!same(filename,json_string(cJSON_GetObjectItemCaseSensitive(record,"document"),"file_name"))) {fail(e,"captured CHUNK filename disagrees with staging");goto done;}
        size_t n=0;unsigned char *bytes=network_download(r,record,TGFS_MAX_CHUNK,&n,e);
        if(!bytes) goto done;
        int good=n==part_size && chunk_digest(bytes,n,record,e) && !memcmp(bytes,data+(size_t)offset,n);free(bytes);
        if(!good) {fail(e,"captured CHUNK bytes or SHA-256 disagree with staging");goto done;}
        if(report_id(verified,json_string(record,"message_id"),e)) goto done;
        seen[index]++;
        if(canonical) {
            const cJSON *part=cJSON_GetArrayItem(parts,(int)index);
            if(same(json_string(part,"message_id"),json_string(record,"message_id"))) {
                const char *next=index+1<count?json_string(cJSON_GetArrayItem(parts,(int)index+1),"message_id"):canonical;
                const cJSON *back=cJSON_GetObjectItemCaseSensitive(c,"file_commit_id");
                if(!back || (!cJSON_IsNull(back) && !same(json_string(c,"file_commit_id"),canonical))) {fail(e,"CHUNK backlink conflicts with canonical FILE_COMMIT");goto done;}
                if(!same(json_string(c,"file_commit_id"),canonical) || !same(json_string(c,"next_message"),next)) {
                    cJSON *fix=cJSON_CreateObject();
                    if(!fix || !cJSON_AddStringToObject(fix,"message_id",json_string(record,"message_id")) ||
                        !cJSON_AddStringToObject(fix,"file_commit_id",canonical) || !cJSON_AddStringToObject(fix,"next_message",next) ||
                        !cJSON_AddItemToArray(repairs,fix)) {cJSON_Delete(fix);fail(e,"out of memory");goto done;}
                }
            }
        }
    }
    for(size_t i=0;i<count;i++) if(seen[i]!=1) {
        cJSON *v=cJSON_CreateNumber((double)i);
        if(!v || !cJSON_AddItemToArray(seen[i]?ambiguous:missing,v)) {cJSON_Delete(v);fail(e,"out of memory");goto done;}
    }
    const cJSON *part;
    cJSON_ArrayForEach(part,cJSON_GetObjectItemCaseSensitive(intent,"parts")) {
        int found=0;const cJSON *id;
        cJSON_ArrayForEach(id,verified) if(same(id->valuestring,json_string(part,"message_id"))) found=1;
        uint64_t offset,n;
        if(json_decimal(part,"offset",&offset) || json_decimal(part,"part_size",&n) || offset>=size || offset%chunk ||
            n!=(size-offset<chunk?size-offset:chunk) || cache_hash(data+(size_t)offset,(size_t)n,hash,e) ||
            !same(hash,json_string(part,"part_sha256"))) {fail(e,"local part ref disagrees with staging");goto done;}
        if(found) cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records"))
            if(same(json_string(record,"message_id"),json_string(part,"message_id"))) {
                const cJSON *c=cJSON_GetObjectItemCaseSensitive(record,"record");
                if(!same(json_string(c,"offset"),json_string(part,"offset")) || !same(json_string(c,"part_sha256"),hash)) {
                    fail(e,"known part ID disagrees with captured range");goto done;
                }
            }
        if(!found && report_id(unknown,json_string(part,"message_id"),e)) goto done;
    }
    if(canonical) cJSON_ArrayForEach(part,parts) {
        int found=0;const cJSON *id;
        cJSON_ArrayForEach(id,verified) if(same(id->valuestring,json_string(part,"message_id"))) found=1;
        if(!found && report_id(unknown,json_string(part,"message_id"),e)) goto done;
    }
    const char *step=cJSON_GetArraySize(ambiguous)?"resolve_duplicate_parts":
        cJSON_GetArraySize(missing)||cJSON_GetArraySize(unknown)?"capture_or_upload_outcome_review":
        !canonical?"commit_publication_review":cJSON_GetArraySize(repairs)?"backlink_repair_review":
        !same(json_string(report,"publication"),"confirmed")?"directory_publication_review":"verified_in_capture";
    if(!cJSON_AddStringToObject(report,"staging","verified") || !cJSON_AddStringToObject(report,"next_step",step)) {fail(e,"out of memory");goto done;}
    rc=0;
done:
    free(seen);free(data);return rc;
}
void tgfs_intent_help(FILE *stream)
{
    fputs("usage: tgfs intent --state-dir DIRECTORY --capture DIRECTORY --token-file PATH [options]\n"
          "Reconcile put/mkdir/unlink intent with a newer completed capture; default is read-only.\n"
          "  --resume-upload          Reuse verified parts, send unattempted parts and FILE_COMMIT.\n"
          "  --finalize               Verify put/mkdir/unlink publication, repair supported navigation, persist complete.\n"
          "  --resume-directory       Resume verified create put or mkdir DIRECTORY publication.\n"
          "  --resume-unlink          Resume an explicitly rejected REMOVE for the same observed ADDs.\n"
          "  --repair-backlinks       Explicitly repair verified CHUNK navigation; logs/locks state-dir.\n"
          "  --state-dir DIRECTORY    Existing private directory containing intent.json.\n"
          "  --capture DIRECTORY      Completed scan newer than source_boundary_id.\n"
          "  --token-file PATH        Owner-only token file for the same bot.\n"
          "  --proxy URL | --direct   Standard transport routing.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  -h, --help               Show help without network.\n"
          "Default: read-only evidence. --repair-backlinks forwards/cleans service copies and edits CHUNK captions.\n"
          "not_observed is not proof of absence; confirmed is not proof of current visibility.\n",stream);
}
int tgfs_intent_main(int argc,char **argv)
{
    int repair=0,resume=0,remove=0,finalize=0,upload=0,log_fd=-1;
    const char *state=NULL,*capture=NULL,*token=NULL,*proxy=NULL,*timeout_text=NULL;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_intent_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--resume-upload")) {if(upload) goto usage;upload=1;continue;}
        if(same(argv[i],"--finalize")) {if(finalize) goto usage;finalize=1;continue;}
        if(same(argv[i],"--resume-unlink")) {if(remove) goto usage;remove=1;continue;}
        if(same(argv[i],"--resume-directory")) {if(resume) goto usage;resume=1;continue;}
        if(same(argv[i],"--repair-backlinks")) {if(repair) goto usage;repair=1;continue;}
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--state-dir")) slot=&state;
        else if(same(argv[i],"--capture")) slot=&capture;
        else if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60;
    if((repair+resume+remove+finalize+upload>1) || !state || !capture || !token || (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300))) goto usage;
    tgfs_error e={0};tgfs_bot_error b;remote r={0};tgfs *fs=NULL;
    cJSON *intent=NULL,*journal=NULL,*report=NULL;int rc=1,dir=-1;
    struct stat st;
    dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0 || fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {fail(&e,"state-dir must be private and owned by this user");goto done;}
    if(fstatat(dir,"intent.json",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        fail(&e,"intent.json must be an owner-only regular file");goto done;
    }
    if(repair || resume || remove || finalize || upload) {
        log_fd=openat(dir,"backlink-repair.log",O_RDWR|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0600);
        if(log_fd<0 || fstat(log_fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_uid!=geteuid() || (st.st_mode&0077) || flock(log_fd,LOCK_EX|LOCK_NB)) {
            fail(&e,"cannot exclusively lock private backlink-repair.log");goto done;
        }
        if(fsync(dir)) {fail(&e,"cannot persist recovery log directory entry");goto done;}
    }
    intent=json_load_at(dir,"intent.json",&e);close(dir);dir=-1;if(!intent) goto done;
    const char *kind=json_string(intent,"kind");
    uint64_t source,boundary;
    if((!same(kind,"put") && !same(kind,"mkdir") && !same(kind,"unlink")) ||
        !uuid_valid(json_string(intent,"object_id")) || !uuid_valid(json_string(intent,"parent_id")) ||
        !json_string(intent,"phase") || json_decimal(intent,"source_boundary_id",&source)) {fail(&e,"unsupported or invalid intent");goto done;}
    dir=open(capture,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {fail(&e,"cannot open capture");goto done;}
    journal=json_load_at(dir,"journal.json",&e);close(dir);dir=-1;if(!journal) goto done;
    if(!same(json_string(intent,"storage_chat_id"),json_string(journal,"storage_chat_id")) ||
        json_decimal(journal,"boundary_id",&boundary) || boundary<=source) {fail(&e,"capture must be newer and from the intent storage channel");goto done;}
    if(tgfs_bot_open(token,(unsigned)timeout,&r.bot,&b) || tgfs_bot_set_proxy(r.bot,proxy,&b)) {api_failure(&e,&b);goto done;}
    /* Existing parser validates the complete journal, origin/channel and merged metadata first. */
    if(read_capture(&r,journal,NULL,NULL,&fs,&e)) goto done;
    report=cJSON_CreateObject();cJSON *commit=report?cJSON_AddArrayToObject(report,"commit_ids"):NULL;
    cJSON *child=report?cJSON_AddArrayToObject(report,"child_ids"):NULL;
    cJSON *directory=report?cJSON_AddArrayToObject(report,"directory_ids"):NULL;
    if(!commit || !child || !directory) {fail(&e,"out of memory");goto done;}
    int commits=0,children=0,dirs=0;
    if(same(kind,"put")) commits=intent_stage(&r,journal,cJSON_GetObjectItemCaseSensitive(intent,"commit_caption"),
        cJSON_GetObjectItemCaseSensitive(intent,"commit_document"),json_string(intent,"operation_id"),commit,&e);
    if(same(kind,"mkdir")) children=intent_stage(&r,journal,cJSON_GetObjectItemCaseSensitive(intent,"child_caption"),
        cJSON_GetObjectItemCaseSensitive(intent,"child_document"),NULL,child,&e);
    const char *op=json_string(intent,same(kind,"unlink")?"remove_operation_id":"add_operation_id");
    if(op) dirs=intent_stage(&r,journal,cJSON_GetObjectItemCaseSensitive(intent,"directory_caption"),
        cJSON_GetObjectItemCaseSensitive(intent,"directory_document"),op,directory,&e);
    if(commits<0 || children<0 || dirs<0) goto done;
    int confirmed=same(kind,"put") ? commits && (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(intent,"replace")) || dirs) :
        same(kind,"mkdir") ? children && dirs : dirs;
    if(!cJSON_AddStringToObject(report,"kind",kind) || !cJSON_AddStringToObject(report,"local_phase",json_string(intent,"phase")) ||
        !cJSON_AddStringToObject(report,"publication",confirmed?"confirmed":"not_observed") ||
        !cJSON_AddStringToObject(report,"scope","provided_capture_only") || !cJSON_AddBoolToObject(report,"resumed",0)) {
        fail(&e,"out of memory");goto done;
    }
    if(intent_data_plan(&r,state,intent,journal,report,&e)) goto done;
    if(upload && resume_upload(r.bot,fs,state,journal,intent,report,log_fd,&e)) goto done;
    if(repair && repair_backlinks(r.bot,journal,intent,report,log_fd,&e)) goto done;
    if(remove && resume_unlink(r.bot,fs,journal,intent,report,log_fd,&e)) goto done;
    if(resume && resume_directory(r.bot,fs,journal,intent,report,log_fd,&e)) goto done;
    if(finalize && (same(kind,"put")?finalize_put(r.bot,state,journal,intent,report,log_fd,&e):
        finalize_metadata(r.bot,state,journal,intent,report,log_fd,&e))) goto done;
    char *raw=cJSON_PrintUnformatted(report);
    if(!raw) {fail(&e,"out of memory");goto done;}
    rc=puts(raw)<0?1:0;free(raw);
done:
    if(rc) fprintf(stderr,"FAIL intent: %s\n",e.message);
    if(dir>=0) close(dir);
    if(log_fd>=0) close(log_fd);
    tgfs_close(fs);tgfs_bot_close(r.bot);cJSON_Delete(intent);cJSON_Delete(journal);cJSON_Delete(report);return rc;
usage:
    tgfs_intent_help(stderr);return 2;
}

static int report_retained_capture(tgfs_remote_session *current,const char *path,
                                   const cJSON *chunk_refs,cJSON *captures,tgfs_error *e)
{
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) return fail(e,"cannot open retained capture: %s",path);
    cJSON *journal=json_load_at(dir,"journal.json",e);
    close(dir);
    if(!journal) return -1;
    const cJSON *current_sb=cJSON_GetObjectItemCaseSensitive(current->journal,"superblock");
    const cJSON *saved_sb=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    uint64_t current_boundary,boundary;
    remote saved={.bot=current->r.bot};
    tgfs *fs=NULL;
    int rc=-1;
    if(!same(json_string(journal,"storage_chat_id"),json_string(current->journal,"storage_chat_id")) ||
        !same(json_string(saved_sb,"root_object_id"),json_string(current_sb,"root_object_id")) ||
        !same(json_string(saved_sb,"start_id"),json_string(current_sb,"start_id"))) {
        fail(e,"retained capture belongs to a different volume: %s",path);goto done;
    }
    if(json_decimal(journal,"boundary_id",&boundary) ||
        json_decimal(current->journal,"boundary_id",&current_boundary) || boundary>=current_boundary) {
        fail(e,"retained capture must have an earlier boundary: %s",path);goto done;
    }
    if(read_capture(&saved,journal,NULL,NULL,&fs,e)) goto done;
    cJSON *capture=cJSON_CreateObject();
    if(!capture || !cJSON_AddItemToArray(captures,capture)) {
        cJSON_Delete(capture);fail(e,"out of memory");goto done;
    }
    cJSON *refs=cJSON_AddArrayToObject(capture,"commit_references");
    if(!refs || !cJSON_AddStringToObject(capture,"capture",path) ||
        !cJSON_AddStringToObject(capture,"boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddBoolToObject(capture,"coverage_verified",cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(journal,"coverage_verified")))) {
        fail(e,"out of memory");goto done;
    }
    rc=tgfs_retained_capture_refs(fs,chunk_refs,refs,e);
done:
    tgfs_close(fs);cJSON_Delete(journal);
    return rc;
}

static int report_saved_parts(const cJSON *parts,cJSON *entry,const char *key,tgfs_error *e)
{
    if(!cJSON_IsArray(parts)) return fail(e,"invalid saved upload parts");
    cJSON *ids=cJSON_AddArrayToObject(entry,key);
    if(!ids) return fail(e,"out of memory");
    const cJSON *part;
    cJSON_ArrayForEach(part,parts) {
        uint64_t id;
        if(json_decimal(part,"message_id",&id) || !id || id>TGFS_BOT_MAX_EXACT_ID)
            return fail(e,"invalid saved upload part ID");
        if(report_id(ids,json_string(part,"message_id"),e)) return -1;
    }
    return 0;
}

static int report_intent_messages(const cJSON *journal,const cJSON *intent,cJSON *entry,tgfs_error *e)
{
    const char *object=json_string(intent,"object_id"),*version=json_string(intent,"version_id");
    const char *operation=json_string(intent,"operation_id");
    cJSON *chunks=cJSON_AddArrayToObject(entry,"observed_chunk_ids");
    cJSON *commits=cJSON_AddArrayToObject(entry,"observed_commit_ids");
    if(!chunks || !commits) return fail(e,"out of memory");
    const cJSON *record;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
        const char *type=json_string(caption,"type");
        int same_version=same(json_string(caption,"object_id"),object) &&
            same(json_string(caption,"version_id"),version);
        if(same(type,"CHUNK") && same_version) {
            if(report_id(chunks,json_string(record,"message_id"),e)) return -1;
        } else if(same(type,"FILE_COMMIT") && (same_version || same(json_string(caption,"operation_id"),operation))) {
            if(report_id(commits,json_string(record,"message_id"),e)) return -1;
        }
    }
    return 0;
}

static int report_retained_intent(const cJSON *journal,const char *path,cJSON *intents,tgfs_error *e)
{
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),log=-1,rc=-1;
    cJSON *intent=NULL;
    struct stat st;
    if(dir<0 || fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        fail(e,"retain-intent requires a private state directory: %s",path);goto done;
    }
    log=openat(dir,"backlink-repair.log",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(log<0 ? errno!=ENOENT : (fstat(log,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&0077) || st.st_nlink!=1 || flock(log,LOCK_SH|LOCK_NB))) {
        fail(e,"cannot read or lock retained intent journal: %s",path);goto done;
    }
    if(!fstatat(dir,"intent.tmp",&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT ||
        !fstatat(dir,"intent.finalize.tmp",&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) {
        fail(e,"retained intent has a pending local state file: %s",path);goto done;
    }
    if(fstatat(dir,"intent.json",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&0077) || st.st_nlink!=1) {
        fail(e,"retained intent must be an owner-only regular file: %s",path);goto done;
    }
    intent=json_load_at(dir,"intent.json",e);
    if(!intent) goto done;
    const char *phase=json_string(intent,"phase"),*object=json_string(intent,"object_id");
    const char *version=json_string(intent,"version_id"),*operation=json_string(intent,"operation_id");
    uint64_t boundary;
    if(!same(json_string(intent,"kind"),"put") || !phase || !*phase || !uuid_valid(object) ||
        !uuid_valid(version) || !uuid_valid(operation) ||
        json_decimal(intent,"source_boundary_id",&boundary) || !boundary || boundary>TGFS_BOT_MAX_EXACT_ID) {
        fail(e,"invalid retained put intent: %s",path);goto done;
    }
    if(!same(json_string(intent,"storage_chat_id"),json_string(journal,"storage_chat_id"))) {
        fail(e,"retained intent belongs to a different storage channel: %s",path);goto done;
    }
    if(same(phase,"complete")) {rc=0;goto done;}
    cJSON *entry=cJSON_CreateObject();
    if(!entry || !cJSON_AddItemToArray(intents,entry)) {
        cJSON_Delete(entry);fail(e,"out of memory");goto done;
    }
    if(!cJSON_AddStringToObject(entry,"state_dir",path) || !cJSON_AddStringToObject(entry,"local_phase",phase) ||
        !cJSON_AddStringToObject(entry,"object_id",object) || !cJSON_AddStringToObject(entry,"version_id",version) ||
        !cJSON_AddStringToObject(entry,"operation_id",operation) || !cJSON_AddBoolToObject(entry,"journal_locked",log>=0) ||
        !cJSON_AddStringToObject(entry,"source_boundary_id",json_string(intent,"source_boundary_id"))) {
        fail(e,"out of memory");goto done;
    }
    const char *keys[]={"file_commit_id","base_commit_id"};
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++) {
        if(!cJSON_HasObjectItem(intent,keys[i])) continue;
        uint64_t id;
        if(json_decimal(intent,keys[i],&id) || !id || id>TGFS_BOT_MAX_EXACT_ID) {
            fail(e,"invalid retained intent commit ID");goto done;
        }
        if(!cJSON_AddStringToObject(entry,keys[i],json_string(intent,keys[i]))) {
            fail(e,"out of memory");goto done;
        }
    }
    const cJSON *manifest=cJSON_GetObjectItemCaseSensitive(intent,"commit_document");
    if(report_saved_parts(cJSON_GetObjectItemCaseSensitive(intent,"parts"),entry,"saved_chunk_ids",e) ||
        (manifest && report_saved_parts(cJSON_GetObjectItemCaseSensitive(manifest,"parts"),entry,"manifest_chunk_ids",e)))
        goto done;
    rc=report_intent_messages(journal,intent,entry,e);
done:
    cJSON_Delete(intent);
    if(log>=0) close(log);
    if(dir>=0) close(dir);
    return rc;
}

static int validate_open_file(const cJSON *file,uint64_t start,uint64_t boundary,tgfs_error *e)
{
    uint64_t commit;
    const cJSON *chunks=cJSON_GetObjectItemCaseSensitive(file,"chunk_ids"),*chunk;
    if(json_decimal(file,"commit_id",&commit) || commit<=start || commit>=boundary || !cJSON_IsArray(chunks))
        return fail(e,"invalid open-file manifest reference");
    cJSON_ArrayForEach(chunk,chunks) {
        uint64_t id;
        if(!cJSON_IsString(chunk) || decimal(chunk->valuestring,&id) || id<=start || id>=commit)
            return fail(e,"invalid open-file CHUNK reference");
    }
    return 0;
}

static int report_open_files(const cJSON *journal,const char *path,cJSON *mounts,tgfs_error *e)
{
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(dir<0 || fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        if(dir>=0) close(dir);
        return fail(e,"retain-open-files requires a private mount state directory: %s",path);
    }
    if(fstatat(dir,"open-files.json",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
        st.st_uid!=geteuid() || (st.st_mode&0077) || st.st_nlink!=1) {
        close(dir);return fail(e,"open-files.json must be an owner-only regular file: %s",path);
    }
    cJSON *saved=json_load_at(dir,"open-files.json",e);
    close(dir);
    if(!saved) return -1;
    const cJSON *sb=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    const cJSON *files=cJSON_GetObjectItemCaseSensitive(saved,"files"),*file;
    uint64_t start,boundary,current_boundary;
    int rc=-1;
    if(!same(json_string(saved,"schema"),"TGFS_OPEN_FILES_V1") || !cJSON_IsArray(files) ||
        json_decimal(saved,"boundary_id",&boundary) || json_decimal(saved,"start_id",&start) ||
        !start || boundary<=start || boundary>TGFS_BOT_MAX_EXACT_ID) {
        fail(e,"invalid open-file report: %s",path);goto done;
    }
    if(!same(json_string(saved,"storage_chat_id"),json_string(journal,"storage_chat_id")) ||
        !same(json_string(saved,"root_object_id"),json_string(sb,"root_object_id")) ||
        !same(json_string(saved,"start_id"),json_string(sb,"start_id"))) {
        fail(e,"open files belong to a different volume: %s",path);goto done;
    }
    if(json_decimal(journal,"boundary_id",&current_boundary) || boundary>current_boundary) {
        fail(e,"open-file report is newer than current capture: %s",path);goto done;
    }
    cJSON_ArrayForEach(file,files) if(validate_open_file(file,start,boundary,e)) goto done;
    cJSON *entry=cJSON_CreateObject();
    if(!entry || !cJSON_AddItemToArray(mounts,entry)) {
        cJSON_Delete(entry);fail(e,"out of memory");goto done;
    }
    if(!cJSON_AddStringToObject(entry,"state_dir",path) ||
        !cJSON_AddStringToObject(entry,"boundary_id",json_string(saved,"boundary_id"))) {
        fail(e,"out of memory");goto done;
    }
    cJSON *owned_files=cJSON_DetachItemFromObjectCaseSensitive(saved,"files");
    if(!cJSON_AddItemToObject(entry,"files",owned_files)) {
        cJSON_Delete(owned_files);fail(e,"out of memory");goto done;
    }
    rc=0;
done:
    cJSON_Delete(saved);
    return rc;
}

typedef enum { GC_CAPTURE, GC_INTENT, GC_OPEN_FILES, GC_INPUT_COUNT } gc_input_kind;
typedef struct {
    const char *path;
    gc_input_kind kind;
} gc_input;

void tgfs_gc_plan_help(FILE *stream)
{
    fputs("usage: tgfs gc-plan --capture DIRECTORY --token-file PATH [options]\n"
          "List content duplicates verified from a completed capture. Reads metadata and CHUNK bytes.\n"
          "  --retain-capture DIRECTORY  Include references from an older capture of this volume; repeatable.\n"
          "  --retain-intent DIRECTORY   Include an unfinished put state directory; repeatable.\n"
          "  --retain-open-files DIR     Include a mount open-file report; repeatable.\n"
          "  --proxy URL | --direct      Transport routing.\n"
          "  --timeout-seconds N         1..300, default 60.\n"
          "Result is advisory and scoped to the supplied captures and local state.\n",stream);
}

int tgfs_gc_plan_main(int argc,char **argv)
{
    tgfs_remote_options options={.no_cache=1,.timeout=60};
    const char *timeout_text=NULL;
    gc_input *inputs=NULL;
    size_t input_count=0;
    int supplied[GC_INPUT_COUNT]={0};
    const char *input_flags[]={"--retain-capture","--retain-intent","--retain-open-files"};
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_gc_plan_help(stdout);return 0;}
    inputs=calloc((size_t)argc,sizeof(*inputs));
    if(!inputs) {fputs("FAIL gc-plan: out of memory\n",stderr);return 1;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--direct")) {if(options.proxy) goto usage;options.proxy="";continue;}
        gc_input_kind kind=GC_CAPTURE;
        while(kind<GC_INPUT_COUNT && !same(argv[i],input_flags[kind])) kind++;
        if(kind<GC_INPUT_COUNT) {
            if(i+1>=argc || !*argv[i+1]) goto usage;
            inputs[input_count++]=(gc_input){argv[++i],kind};
            supplied[kind]=1;
            continue;
        }
        if(same(argv[i],"--capture")) slot=&options.capture;
        else if(same(argv[i],"--token-file")) slot=&options.token_file;
        else if(same(argv[i],"--proxy")) slot=&options.proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60;
    if(!options.capture || !options.token_file || (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300)))
        goto usage;
    options.timeout=(unsigned)timeout;
    tgfs_error e={0};
    tgfs_remote_session *session=NULL;
    cJSON *report=NULL;
    char *json=NULL;
    int result=1;
    if(tgfs_remote_open(&options,&session,&e)) goto done;
    const cJSON *journal=session->journal;
    const cJSON *superblock=cJSON_GetObjectItemCaseSensitive(journal,"superblock");
    report=cJSON_CreateObject();
    cJSON *groups=report?cJSON_AddArrayToObject(report,"groups"):NULL;
    if(!groups || !cJSON_AddStringToObject(report,"schema","TGFS_DUPLICATE_REPORT_V1") ||
        !cJSON_AddStringToObject(report,"scope",supplied[GC_OPEN_FILES]?"provided_captures_and_local_state_only":
            supplied[GC_INTENT]?"provided_captures_and_intents_only":
            supplied[GC_CAPTURE]?"provided_captures_only":"provided_capture_only") ||
        !cJSON_AddStringToObject(report,"storage_chat_id",json_string(journal,"storage_chat_id")) ||
        !cJSON_AddStringToObject(report,"root_object_id",json_string(superblock,"root_object_id")) ||
        !cJSON_AddStringToObject(report,"boundary_id",json_string(journal,"boundary_id")) ||
        !cJSON_AddBoolToObject(report,"coverage_verified",cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(journal,"coverage_verified")))) {
        fail(&e,"out of memory");goto done;
    }
    cJSON *chunk_refs=cJSON_AddArrayToObject(report,"chunk_references");
    if(!chunk_refs) {fail(&e,"out of memory");goto done;}
    if(tgfs_duplicate_groups(session->fs,groups,chunk_refs,&e)) goto done;
    cJSON *retained[GC_INPUT_COUNT]={0};
    const char *keys[]={"retained_captures","retained_intents","retained_open_files"};
    for(size_t i=0;i<GC_INPUT_COUNT;i++) {
        if(supplied[i] && !(retained[i]=cJSON_AddArrayToObject(report,keys[i]))) {
            fail(&e,"out of memory");goto done;
        }
    }
    for(size_t i=0;i<input_count;i++) {
        const char *path=inputs[i].path;
        switch(inputs[i].kind) {
        case GC_CAPTURE:
            if(report_retained_capture(session,path,chunk_refs,retained[GC_CAPTURE],&e)) goto done;
            break;
        case GC_INTENT:
            if(report_retained_intent(journal,path,retained[GC_INTENT],&e)) goto done;
            break;
        case GC_OPEN_FILES:
            if(report_open_files(journal,path,retained[GC_OPEN_FILES],&e)) goto done;
            break;
        case GC_INPUT_COUNT:
            fail(&e,"unsupported gc-plan input");goto done;
        }
    }
    if(tgfs_gc_retention_reasons(session->fs,report,&e)) goto done;
    json=cJSON_PrintUnformatted(report);
    if(!json) {fail(&e,"out of memory");goto done;}
    if(puts(json)<0 || fflush(stdout)) {fail(&e,"cannot write duplicate report");goto done;}
    result=0;
done:
    if(result) fprintf(stderr,"FAIL gc-plan: %s\n",e.message);
    free(inputs);free(json);cJSON_Delete(report);tgfs_remote_close(session);
    return result;
usage:
    free(inputs);
    tgfs_gc_plan_help(stderr);return 2;
}
