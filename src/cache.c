#include "cache.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_APP_ID 1413957203
struct tgfs_cache {sqlite3 *db;size_t limit;};
static int db_error(tgfs_cache *c,tgfs_error *e)
{
    return fail(e,"cache database error (sqlite=%d); use --no-cache or a separate private --cache-dir",
        sqlite3_extended_errcode(c->db));
}
static int execute(tgfs_cache *c,const char *sql,tgfs_error *e)
{
    return sqlite3_exec(c->db,sql,NULL,NULL,NULL)==SQLITE_OK ? 0 : db_error(c,e);
}
static int prepare(tgfs_cache *c,const char *sql,sqlite3_stmt **s,tgfs_error *e)
{
    return sqlite3_prepare_v2(c->db,sql,-1,s,NULL)==SQLITE_OK ? 0 : db_error(c,e);
}
int cache_hash(const void *bytes,size_t length,char hash[65],tgfs_error *e)
{
    unsigned char result[32];unsigned n;
    if(!EVP_Digest(bytes,length,result,&n,EVP_sha256(),NULL) || n!=32) return fail(e,"cannot hash cache data");
    for(size_t i=0;i<32;i++) snprintf(hash+i*2,3,"%02x",result[i]);
    return 0;
}
int cache_json_key(const cJSON *value,char key[65],tgfs_error *e)
{
    char *raw=cJSON_PrintUnformatted(value);
    if(!raw) return fail(e,"out of memory");
    int rc=cache_hash(raw,strlen(raw),key,e);free(raw);return rc;
}
static char *location(const char *directory,tgfs_error *e)
{
    const char *base=directory;char *owned=NULL;
    if(!base) {
        const char *xdg=getenv("XDG_CACHE_HOME"),*home=getenv("HOME");
        if(xdg && xdg[0]=='/') base=xdg;
        else if(home && home[0]=='/') {
            size_t n=strlen(home)+8;owned=malloc(n);
            if(owned) snprintf(owned,n,"%s/.cache",home);
            base=owned;
        } else {fail(e,"absolute HOME or XDG_CACHE_HOME required for default cache");return NULL;}
        if(!base) {fail(e,"out of memory");return NULL;}
        size_t n=strlen(base)+6;char *next=malloc(n);
        if(next) snprintf(next,n,"%s/tgfs",base);
        free(owned);owned=next;base=owned;
    }
    if(!base || !*base || strlen(base)>4000) {free(owned);fail(e,"invalid cache directory");return NULL;}
    char *path=strdup(base);free(owned);
    if(path) {size_t n=strlen(path);while(n>1 && path[n-1]=='/') path[--n]=0;}
    if(!path) {fail(e,"out of memory");return NULL;}
    for(char *p=path+1;*p;p++) if(*p=='/') {
        *p=0;int rc=mkdir(path,0700);*p='/';
        if(rc && errno!=EEXIST) {free(path);fail(e,"cannot create cache parent");return NULL;}
    }
    if(mkdir(path,0700) && errno!=EEXIST) {free(path);fail(e,"cannot create cache directory");return NULL;}
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(dir<0 || fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&077)) {
        if(dir>=0) close(dir);
        free(path);fail(e,"cache directory must be owner-only and not a symlink");return NULL;
    }
    int fd=openat(dir,"index.sqlite3",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    close(dir);
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)) {
        if(fd>=0) close(fd);
        free(path);fail(e,"cache database must be an owner-only regular file");return NULL;
    }
    close(fd);
    /* Canonical path avoids SQLite NOFOLLOW rejecting a symlink in HOME. */
    char *absolute=realpath(path,NULL);free(path);
    if(!absolute) {fail(e,"cannot resolve cache directory");return NULL;}
    size_t n=strlen(absolute)+16;char *dbpath=malloc(n);
    if(dbpath) snprintf(dbpath,n,"%s/index.sqlite3",absolute);
    free(absolute);
    if(!dbpath) fail(e,"out of memory");
    return dbpath;
}
static int scalar(tgfs_cache *c,const char *sql,sqlite3_int64 *value,tgfs_error *e)
{
    sqlite3_stmt *s=NULL;
    if(prepare(c,sql,&s,e)) return -1;
    int rc=sqlite3_step(s);
    if(rc==SQLITE_ROW) *value=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);return rc==SQLITE_ROW ? 0 : db_error(c,e);
}
static int trim(tgfs_cache *c,tgfs_error *e)
{
    sqlite3_stmt *s=NULL;
    if(prepare(c,"DELETE FROM blobs WHERE length(payload)>?",&s,e)) return -1;
    int rc=sqlite3_bind_int64(s,1,(sqlite3_int64)c->limit);
    if(rc==SQLITE_OK) rc=sqlite3_step(s);
    sqlite3_finalize(s);
    if(rc!=SQLITE_DONE) return db_error(c,e);
    for(;;) {
        sqlite3_int64 total;
        if(scalar(c,"SELECT coalesce(sum(length(payload)),0) FROM blobs",&total,e)) return -1;
        if((uint64_t)total<=c->limit) return 0;
        if(execute(c,"DELETE FROM blobs WHERE key=(SELECT key FROM blobs ORDER BY touched LIMIT 1)",e)) return -1;
    }
}
int cache_open(const char *directory,size_t limit,tgfs_cache **out,tgfs_error *e)
{
    *out=NULL;
    if(sqlite3_libversion_number()<3031000) return fail(e,"SQLite >= 3.31 required");
    char *path=location(directory,e);if(!path) return -1;
    tgfs_cache *c=calloc(1,sizeof(*c));if(!c) {free(path);return fail(e,"out of memory");}
    int opened=sqlite3_open_v2(path,&c->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW|SQLITE_OPEN_FULLMUTEX,NULL);
    free(path);
    if(opened!=SQLITE_OK) {db_error(c,e);cache_close(c);return -1;}
    c->limit=limit;
    sqlite3_busy_timeout(c->db,5000);
    sqlite3_limit(c->db,SQLITE_LIMIT_LENGTH,32*1024*1024);
    /* Serialize schema/version inspection with initialization and quota changes.
     * Reading the two PRAGMAs in separate autocommit snapshots races an opener. */
    if(execute(c,"PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY; BEGIN IMMEDIATE;",e)) goto invalid;
    sqlite3_int64 app=0,version=0,tables=0;
    if(scalar(c,"PRAGMA application_id",&app,e) || scalar(c,"PRAGMA user_version",&version,e)) goto invalid;
    if(!app && !version) {
        if(scalar(c,"SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'",&tables,e)) goto invalid;
        if(tables) {fail(e,"cache path contains another SQLite database");goto invalid;}
        if(execute(c,
            "CREATE TABLE blobs(key TEXT PRIMARY KEY,payload BLOB NOT NULL,sha TEXT NOT NULL,touched INTEGER NOT NULL);"
            "CREATE TABLE captures(key TEXT PRIMARY KEY,snapshot BLOB NOT NULL,sha TEXT NOT NULL,touched INTEGER NOT NULL);"
            "CREATE TABLE records(capture_key TEXT NOT NULL REFERENCES captures(key) ON DELETE CASCADE,"
            "source_id INTEGER NOT NULL,object_id TEXT,type TEXT NOT NULL,json TEXT NOT NULL,PRIMARY KEY(capture_key,source_id));"
            "CREATE INDEX records_by_object ON records(capture_key,object_id,source_id);"
            "PRAGMA application_id=1413957203; PRAGMA user_version=1;",e)) goto invalid;
    } else if(app!=CACHE_APP_ID || version!=1) {fail(e,"unsupported cache database schema");goto invalid;}
    sqlite3_int64 total;
    if(scalar(c,"SELECT coalesce(sum(length(payload)),0) FROM blobs",&total,e)) goto invalid;
    if((uint64_t)total>limit && trim(c,e)) goto invalid;
    if(execute(c,"COMMIT",e)) goto invalid;
    *out=c;return 0;
invalid:
    sqlite3_exec(c->db,"ROLLBACK",NULL,NULL,NULL);cache_close(c);return -1;
}
void cache_close(tgfs_cache *c) {if(c) {sqlite3_close(c->db);free(c);}}
static int drop(tgfs_cache *c,const char *sql,const char *key,tgfs_error *e)
{
    sqlite3_stmt *s=NULL;if(prepare(c,sql,&s,e)) return -1;
    int rc=sqlite3_bind_text(s,1,key,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_step(s);
    sqlite3_finalize(s);return rc==SQLITE_DONE ? 0 : db_error(c,e);
}
int cache_blob_drop(tgfs_cache *c,const char *key,tgfs_error *e)
{
    return drop(c,"DELETE FROM blobs WHERE key=?",key,e);
}
static int get(tgfs_cache *c,const char *sql,const char *key,size_t limit,unsigned char **bytes,size_t *length,tgfs_error *e)
{
    *bytes=NULL;*length=0;
    sqlite3_stmt *s=NULL;if(prepare(c,sql,&s,e)) return -1;
    int rc=sqlite3_bind_text(s,1,key,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_step(s);
    if(rc==SQLITE_DONE) {sqlite3_finalize(s);return 0;}
    if(rc!=SQLITE_ROW) {sqlite3_finalize(s);return db_error(c,e);}
    int n=sqlite3_column_bytes(s,0);
    const unsigned char *data=sqlite3_column_blob(s,0),*stored=sqlite3_column_text(s,1);
    char hash[65];
    int valid=n>=0 && (size_t)n<=limit && (data || !n) && stored &&
        !cache_hash(data,(size_t)n,hash,e) && !strcmp(hash,(const char *)stored);
    if(valid) {
        *bytes=malloc((size_t)n+1);
        if(!*bytes) {sqlite3_finalize(s);return fail(e,"out of memory");}
        if(n) memcpy(*bytes,data,(size_t)n);
        (*bytes)[n]=0;*length=(size_t)n;
    }
    sqlite3_finalize(s);return valid ? 1 : 0;
}
int cache_blob_get(tgfs_cache *c,const char *key,size_t limit,unsigned char **bytes,size_t *length,tgfs_error *e)
{
    int rc=get(c,"SELECT payload,sha FROM blobs WHERE key=?",key,limit,bytes,length,e);
    if(rc==0) {if(cache_blob_drop(c,key,e)) return -1;}
    else if(rc==1 && drop(c,"UPDATE blobs SET touched=(SELECT coalesce(max(touched),0)+1 FROM blobs) WHERE key=?",key,e)) {
        free(*bytes);*bytes=NULL;return -1;
    }
    return rc;
}
int cache_blob_put(tgfs_cache *c,const char *key,const unsigned char *bytes,size_t length,tgfs_error *e)
{
    if(length>c->limit) return 0;
    char hash[65];if(cache_hash(bytes,length,hash,e)) return -1;
    if(execute(c,"BEGIN IMMEDIATE",e)) return -1;
    sqlite3_stmt *s=NULL;
    if(prepare(c,"INSERT OR REPLACE INTO blobs VALUES(?,?,?,(SELECT coalesce(max(touched),0)+1 FROM blobs))",&s,e)) goto invalid;
    int rc=sqlite3_bind_text(s,1,key,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_bind_blob64(s,2,bytes,length,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_bind_text(s,3,hash,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_step(s);
    sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE) {db_error(c,e);goto invalid;}
    if(trim(c,e)) goto invalid;
    if(execute(c,"COMMIT",e)) goto invalid;
    return 0;
invalid:
    sqlite3_finalize(s);sqlite3_exec(c->db,"ROLLBACK",NULL,NULL,NULL);return -1;
}
int cache_index_get(tgfs_cache *c,const char *key,cJSON **snapshot,tgfs_error *e)
{
    unsigned char *bytes=NULL;size_t length=0;
    int rc=get(c,"SELECT snapshot,sha FROM captures WHERE key=?",key,TGFS_MAX_JSON,&bytes,&length,e);
    if(rc==1) {
        *snapshot=json_parse_bytes(bytes,length,e);free(bytes);
        if(*snapshot) {
            if(drop(c,"UPDATE captures SET touched=(SELECT coalesce(max(touched),0)+1 FROM captures) WHERE key=?",key,e)) {
                cJSON_Delete(*snapshot);*snapshot=NULL;return -1;
            }
            return 1;
        }
        if(e) memset(e,0,sizeof(*e));
        rc=0;
    }
    if(!rc && drop(c,"DELETE FROM captures WHERE key=?",key,e)) return -1;
    return rc;
}
int cache_index_put(tgfs_cache *c,const char *key,const cJSON *snapshot,const cJSON *journal,tgfs_error *e)
{
    char *raw=cJSON_PrintUnformatted(snapshot);char hash[65];sqlite3_stmt *s=NULL;
    if(!raw) return fail(e,"out of memory");
    size_t size=strlen(raw);
    if(size>TGFS_MAX_JSON || cache_hash(raw,size,hash,e)) {free(raw);return fail(e,"cache index size/hash failed");}
    if(execute(c,"BEGIN IMMEDIATE",e)) {free(raw);return -1;}
    if(prepare(c,"INSERT OR REPLACE INTO captures VALUES(?,?,?,(SELECT coalesce(max(touched),0)+1 FROM captures))",&s,e)) goto invalid;
    int rc=sqlite3_bind_text(s,1,key,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_bind_blob64(s,2,raw,size,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_bind_text(s,3,hash,-1,SQLITE_TRANSIENT);
    if(rc==SQLITE_OK) rc=sqlite3_step(s);
    sqlite3_finalize(s);s=NULL;
    if(rc!=SQLITE_DONE) {db_error(c,e);goto invalid;}
    const cJSON *record;
    cJSON_ArrayForEach(record,cJSON_GetObjectItemCaseSensitive(journal,"records")) {
        const cJSON *caption=cJSON_GetObjectItemCaseSensitive(record,"record");
        uint64_t id;if(json_decimal(record,"message_id",&id)) {fail(e,"invalid index source ID");goto invalid;}
        char *json=cJSON_PrintUnformatted(record);
        if(!json) {fail(e,"out of memory");goto invalid;}
        if(prepare(c,"INSERT INTO records VALUES(?,?,?,?,?)",&s,e)) {free(json);goto invalid;}
        rc=sqlite3_bind_text(s,1,key,-1,SQLITE_TRANSIENT);
        if(rc==SQLITE_OK) rc=sqlite3_bind_int64(s,2,(sqlite3_int64)id);
        const char *object=json_string(caption,"object_id"),*type=json_string(caption,"type");
        if(rc==SQLITE_OK) rc=object?sqlite3_bind_text(s,3,object,-1,SQLITE_TRANSIENT):sqlite3_bind_null(s,3);
        if(rc==SQLITE_OK) rc=sqlite3_bind_text(s,4,type,-1,SQLITE_TRANSIENT);
        if(rc==SQLITE_OK) rc=sqlite3_bind_text(s,5,json,-1,SQLITE_TRANSIENT);
        if(rc==SQLITE_OK) rc=sqlite3_step(s);
        free(json);sqlite3_finalize(s);s=NULL;
        if(rc!=SQLITE_DONE) {db_error(c,e);goto invalid;}
    }
    if(execute(c,"DELETE FROM captures WHERE key NOT IN (SELECT key FROM captures ORDER BY touched DESC LIMIT 16)",e) ||
        execute(c,"COMMIT",e)) goto invalid;
    free(raw);return 0;
invalid:
    free(raw);sqlite3_finalize(s);sqlite3_exec(c->db,"ROLLBACK",NULL,NULL,NULL);return -1;
}
