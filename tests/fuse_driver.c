#include "../src/fuse_backend.h"
#include "../src/internal.h"
#include "tgfs/remote.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {int dir;uint64_t ids[256];size_t count;} loader_state;
static unsigned char *load(uint64_t id,size_t *length,void *arg,tgfs_error *e)
{
    loader_state *s=arg;
    if(s->count>=256) {fail(e,"too many test loads");return NULL;}
    s->ids[s->count++]=id;
    char name[48];snprintf(name,sizeof(name),"%" PRIu64 ".bin",id);
    return bytes_load_at(s->dir,name,TGFS_MAX_CHUNK,length,e);
}
static void number(cJSON *o,const char *name,long long n) {cJSON_AddNumberToObject(o,name,(double)n);}
static void bytes_result(cJSON *o,const char *key,const unsigned char *bytes,int n)
{
    size_t size=n>0?(size_t)n:0;char *hex=calloc(size*2+1,1);
    if(!hex) exit(2);
    for(size_t i=0;i<size;i++) snprintf(hex+i*2,3,"%02x",bytes[i]);
    cJSON_AddStringToObject(o,key,hex);free(hex);
}
typedef struct {cJSON *names;size_t left;off_t last;} page;
static int filler(void *arg,const char *name,const struct stat *st,off_t offset,enum fuse_fill_dir_flags flags)
{
    (void)st;(void)flags;page *p=arg;
    if(!p->left) return 1;
    p->left--;p->last=offset;cJSON_AddItemToArray(p->names,cJSON_CreateString(name));return 0;
}
int main(int argc,char **argv)
{
    if(argc<3) return 2;
    loader_state state={.dir=-1};tgfs *fs=NULL;tgfs_remote_session *session=NULL;tgfs_error e={0};
    const char *capture=getenv("TGFS_DRIVER_CAPTURE");
    if(capture) {
        tgfs_remote_options o={.capture=capture,.token_file=getenv("TGFS_DRIVER_TOKEN"),
            .cache_dir=getenv("TGFS_DRIVER_CACHE"),.timeout=2,.cache_limit=256U*1024U*1024U,
            .offline=getenv("TGFS_DRIVER_OFFLINE")!=NULL};
        if(tgfs_remote_open(&o,&session,&e)) {fprintf(stderr,"driver: %s\n",e.message);return 1;}
        fs=tgfs_remote_fs(session);
    } else {
        state.dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(state.dir<0 || tgfs_open_snapshot_loader(argv[1],load,&state,&fs,&e)) {
            fprintf(stderr,"driver: %s\n",e.message);if(state.dir>=0) close(state.dir);return 1;
        }
    }
    tgfs_fuse_view *v=tgfs_fuse_view_new(fs,&e);
    if(!v) {fprintf(stderr,"driver view: %s\n",e.message);if(session) tgfs_remote_close(session);else tgfs_close(fs);if(state.dir>=0) close(state.dir);return 1;}
    tgfs_fuse_test_context(v);const struct fuse_operations *ops=tgfs_fuse_operations();
    cJSON *result=cJSON_CreateObject();const char *path="/documents/hello.txt";
    struct fuse_file_info fi={.flags=O_RDONLY};
    unsigned char buffer[64]={0};
    if(!strcmp(argv[2],"basic")) {
        struct stat st;
        number(result,"root_rc",ops->getattr("/",&st,NULL));number(result,"root_inode",(long long)st.st_ino);
        number(result,"root_links",(long long)st.st_nlink);number(result,"root_mode",st.st_mode&0777);
        number(result,"file_rc",ops->getattr(path,&st,NULL));number(result,"file_size",st.st_size);
        number(result,"file_inode",(long long)st.st_ino);number(result,"file_mode",st.st_mode&0777);
        number(result,"missing",ops->getattr("/missing",&st,NULL));
        number(result,"notdir",ops->getattr("/documents/hello.txt/child",&st,NULL));
        number(result,"isdir",ops->open("/documents",&fi));
        number(result,"opendir_file",ops->opendir(path,&fi));
        number(result,"write_access",ops->access(path,W_OK));number(result,"exec_access",ops->access(path,X_OK));
        number(result,"read_access",ops->access(path,R_OK));
        cJSON *names=cJSON_CreateArray();off_t offset=0;
        for(int i=0;i<8;i++) {
            page p={.names=names,.left=1,.last=offset};
            int rc=ops->readdir("/documents",&p,filler,offset,NULL,FUSE_READDIR_PLUS);
            if(rc || p.last==offset) {number(result,"readdir_rc",rc);break;}
            offset=p.last;
        }
        cJSON_AddItemToObject(result,"names",names);
        number(result,"open",ops->open(path,&fi));number(result,"direct_io",fi.direct_io);
        number(result,"release",ops->release(path,&fi));number(result,"stale_read",ops->read(path,(char *)buffer,1,0,&fi));
    } else if(!strcmp(argv[2],"writes")) {
        struct fuse_file_info w={.flags=O_WRONLY},rw={.flags=O_RDWR},trunc={.flags=O_RDONLY|O_TRUNC};
        number(result,"open_write",ops->open(path,&w));number(result,"open_rdwr",ops->open(path,&rw));
        number(result,"open_truncate",ops->open(path,&trunc));
        number(result,"mknod",ops->mknod("/new",0600,0));number(result,"mkdir",ops->mkdir("/new",0700));
        number(result,"unlink",ops->unlink(path));number(result,"rmdir",ops->rmdir("/documents"));
        number(result,"rename",ops->rename(path,"/new",0));number(result,"link",ops->link(path,"/new"));
        number(result,"symlink",ops->symlink(path,"/new"));number(result,"chmod",ops->chmod(path,0600,NULL));
        number(result,"chown",ops->chown(path,0,0,NULL));number(result,"truncate",ops->truncate(path,0,NULL));
        number(result,"create",ops->create("/new",0600,&w));number(result,"write",ops->write(path,"X",1,0,NULL));
        number(result,"setxattr",ops->setxattr(path,"user.test","x",1,0));number(result,"removexattr",ops->removexattr(path,"user.test"));
        number(result,"utimens",ops->utimens(path,NULL,NULL));number(result,"fallocate",ops->fallocate(path,0,0,1,NULL));
    } else {
        if(!strcmp(argv[2],"empty")) path="/documents/notes.txt";
        int opened=ops->open(path,&fi);number(result,"open",opened);
        if(!opened) {
            if(!strcmp(argv[2],"range")) {
                if(argc!=5) return 2;
                off_t offset=(off_t)strtoll(argv[3],NULL,10);size_t size=(size_t)strtoull(argv[4],NULL,10);
                if(size>64 && size<=INT_MAX) return 2;
                int n=ops->read(path,(char *)buffer,size,offset,&fi);
                number(result,"read",n);bytes_result(result,"hex",buffer,n);
            } else if(!strcmp(argv[2],"sequence")) {
                cJSON *reads=cJSON_CreateArray(),*hex=cJSON_CreateArray();
                for(off_t offset=0;offset<6;offset+=2) {
                    int n=ops->read(path,(char *)buffer,2,offset,&fi);
                    cJSON_AddItemToArray(reads,cJSON_CreateNumber(n));
                    cJSON *item=cJSON_CreateObject();bytes_result(item,"value",buffer,n);
                    cJSON_AddItemToArray(hex,cJSON_DetachItemFromObjectCaseSensitive(item,"value"));cJSON_Delete(item);
                }
                cJSON_AddItemToObject(result,"reads",reads);cJSON_AddItemToObject(result,"hex",hex);
                number(result,"again",ops->read(path,(char *)buffer,1,0,&fi));
            } else if(!strcmp(argv[2],"ram")) {
                number(result,"first",ops->read(path,(char *)buffer,1,0,&fi));
                number(result,"second",ops->read(path,(char *)buffer,1,1,&fi));
                struct fuse_file_info other={.flags=O_RDONLY};
                number(result,"other_open",ops->open(path,&other));
                number(result,"other_read",ops->read(path,(char *)buffer,1,1,&other));
                number(result,"third",ops->read(path,(char *)buffer,1,3,&fi));
                number(result,"fourth",ops->read(path,(char *)buffer,1,4,&fi));
                /* Deliberately leave other handle for view teardown. */
            } else if(!strcmp(argv[2],"immutable")) {
                unlinkat(state.dir,"snapshot.json",0);unlinkat(state.dir,"110.json",0);
                int n=ops->read(path,(char *)buffer,6,0,&fi);number(result,"read",n);bytes_result(result,"hex",buffer,n);
            } else {
                int n=ops->read(path,(char *)buffer,64,0,&fi);number(result,"read",n);bytes_result(result,"hex",buffer,n);
            }
            ops->release(path,&fi);
        }
    }
    cJSON *loads=cJSON_CreateArray();
    for(size_t i=0;i<state.count;i++) cJSON_AddItemToArray(loads,cJSON_CreateNumber((double)state.ids[i]));
    cJSON_AddItemToObject(result,"loads",loads);
    char *raw=cJSON_PrintUnformatted(result);if(raw) puts(raw);free(raw);cJSON_Delete(result);
    tgfs_fuse_test_context(NULL);tgfs_fuse_view_close(v);
    if(session) {tgfs_remote_report(session,stderr);tgfs_remote_close(session);}else tgfs_close(fs);
    if(state.dir>=0) close(state.dir);
    return 0;
}
