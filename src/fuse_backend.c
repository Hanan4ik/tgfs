#include "fuse_backend.h"
#include "internal.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct open_file { uint64_t id;tgfs_file *file;struct open_file *next; } open_file;
struct tgfs_fuse_view {tgfs *fs;open_file *files;uint64_t next_handle;};
#ifdef TGFS_TESTING
static tgfs_fuse_view *test_context;
void tgfs_fuse_test_context(tgfs_fuse_view *v) {test_context=v;}
#endif
static tgfs_fuse_view *view(void)
{
#ifdef TGFS_TESTING
    if(test_context) return test_context;
#endif
    struct fuse_context *ctx=fuse_get_context();return ctx?ctx->private_data:NULL;
}
static int error_result(const tgfs_error *e) {return -(e->code?e->code:EIO);}
static int ignore_entry(const char *name,const char *object,void *arg)
{(void)name;(void)object;(void)arg;return 0;}
tgfs_fuse_view *tgfs_fuse_view_new(tgfs *fs,tgfs_error *e)
{
    if(tgfs_list(fs,"/",ignore_entry,NULL,e)) return NULL;
    tgfs_fuse_view *v=calloc(1,sizeof(*v));
    if(!v) {fail_code(e,ENOMEM,"out of memory opening mount view");return NULL;}
    v->fs=fs;v->next_handle=1;return v;
}
void tgfs_fuse_view_close(tgfs_fuse_view *v)
{
    if(!v) return;
    while(v->files) {open_file *f=v->files;v->files=f->next;tgfs_file_close(f->file);free(f);}
    free(v);
}
static open_file *find_file(tgfs_fuse_view *v,uint64_t id)
{
    if(v) for(open_file *f=v->files;f;f=f->next) if(f->id==id) return f;
    return NULL;
}
static void fill_stat(const tgfs_attr *a,struct stat *st)
{
    memset(st,0,sizeof(*st));st->st_ino=(ino_t)a->inode;
    st->st_mode=a->directory?(S_IFDIR|0555):(S_IFREG|0444);
    st->st_nlink=(nlink_t)a->links;st->st_uid=geteuid();st->st_gid=getegid();
    st->st_size=(off_t)a->size;st->st_blocks=(blkcnt_t)((a->size+511)/512);st->st_blksize=4096;
}
static int ro_getattr(const char *path,struct stat *st,struct fuse_file_info *fi)
{
    tgfs_fuse_view *v=view();if(!v) return -EIO;
    tgfs_attr attr;tgfs_error e={0};
    if(fi && fi->fh) {
        open_file *f=find_file(v,fi->fh);if(!f) return -EBADF;tgfs_file_attr(f->file,&attr);
    } else if(tgfs_getattr(v->fs,path,&attr,&e)) return error_result(&e);
    fill_stat(&attr,st);return 0;
}
static int ro_open(const char *path,struct fuse_file_info *fi)
{
    if(!fi) return -EINVAL;
    if((fi->flags&O_ACCMODE)!=O_RDONLY || (fi->flags&(O_TRUNC|O_APPEND|O_CREAT|O_EXCL))) return -EROFS;
    tgfs_fuse_view *v=view();if(!v) return -EIO;
    open_file *f=calloc(1,sizeof(*f));if(!f) return -ENOMEM;
    tgfs_error e={0};
    if(tgfs_file_open(v->fs,path,&f->file,&e)) {free(f);return error_result(&e);}
    if(fi->flags&O_DIRECTORY) {tgfs_file_close(f->file);free(f);return -ENOTDIR;}
    if(!v->next_handle) {tgfs_file_close(f->file);free(f);return -EMFILE;}
    f->id=v->next_handle++;f->next=v->files;v->files=f;fi->fh=f->id;
    fi->direct_io=1;return 0;
}
static int ro_read(const char *path,char *buffer,size_t size,off_t offset,struct fuse_file_info *fi)
{
    (void)path;
    if(offset<0 || size>INT_MAX) return -EINVAL;
    open_file *f=fi?find_file(view(),fi->fh):NULL;if(!f) return -EBADF;
    size_t length=0;tgfs_error e={0};
    if(tgfs_file_read(f->file,(uint64_t)offset,buffer,size,&length,&e)) {
        fprintf(stderr,"tgfs read: %s\n",e.message);return error_result(&e);
    }
    return (int)length;
}
static int ro_release(const char *path,struct fuse_file_info *fi)
{
    (void)path;tgfs_fuse_view *v=view();if(!v || !fi) return -EBADF;
    open_file **p=&v->files;while(*p && (*p)->id!=fi->fh) p=&(*p)->next;
    if(!*p) return -EBADF;
    open_file *f=*p;*p=f->next;tgfs_file_close(f->file);free(f);fi->fh=0;return 0;
}
static int ro_opendir(const char *path,struct fuse_file_info *fi)
{
    tgfs_fuse_view *v=view();tgfs_attr a;tgfs_error e={0};if(!v) return -EIO;
    if(tgfs_getattr(v->fs,path,&a,&e)) return error_result(&e);
    if(!a.directory) return -ENOTDIR;
    if(fi) fi->fh=0;
    return 0;
}
typedef struct {
    tgfs_fuse_view *view;void *buffer;fuse_fill_dir_t filler;
    off_t offset,position;int full,error;enum fuse_fill_dir_flags flags;
} directory_output;
static int fill_entry(directory_output *o,const char *name,const struct stat *st)
{
    o->position++;
    if(o->position<=o->offset) return 0;
    if(o->filler(o->buffer,name,st,o->position,o->flags)) {o->full=1;return 1;}
    return 0;
}
static int directory_entry(const char *name,const char *object,void *arg)
{
    directory_output *o=arg;
    if(o->position+1<=o->offset) {o->position++;return 0;}
    tgfs_error e={0};tgfs_attr a;struct stat st;
    if(tgfs_object_attr(o->view->fs,object,&a,&e)) {o->error=error_result(&e);return 1;}
    fill_stat(&a,&st);return fill_entry(o,name,&st);
}
static int ro_readdir(const char *path,void *buffer,fuse_fill_dir_t filler,off_t offset,
                      struct fuse_file_info *fi,enum fuse_readdir_flags flags)
{
    if(offset<0) return -EINVAL;
    int rc=ro_opendir(path,fi);if(rc) return rc;
    tgfs_fuse_view *v=view();
    directory_output out={.view=v,.buffer=buffer,.filler=filler,.offset=offset,
        .flags=(flags&FUSE_READDIR_PLUS)?FUSE_FILL_DIR_PLUS:0};
    struct stat st;
    rc=ro_getattr(path,&st,NULL);if(rc) return rc;
    if(fill_entry(&out,".",&st)) return 0;
    char *parent=strdup(path);if(!parent) return -ENOMEM;
    size_t parent_length=strlen(parent);
    while(parent_length>1 && parent[parent_length-1]=='/') parent[--parent_length]=0;
    char *slash=strrchr(parent,'/');
    if(slash && slash!=parent) *slash=0;else {free(parent);parent=strdup("/");}
    if(!parent) return -ENOMEM;
    rc=ro_getattr(parent,&st,NULL);free(parent);if(rc) return rc;
    if(fill_entry(&out,"..",&st)) return 0;
    tgfs_error e={0};rc=tgfs_list(v->fs,path,directory_entry,&out,&e);
    if(out.error) return out.error;
    if(out.full) return 0;
    return rc?error_result(&e):0;
}
static int ro_access(const char *path,int mask)
{
    if(mask&W_OK) return -EROFS;
    struct stat st;int rc=ro_getattr(path,&st,NULL);if(rc) return rc;
    if((mask&X_OK) && !S_ISDIR(st.st_mode)) return -EACCES;
    return 0;
}
static int ro_statfs(const char *path,struct statvfs *st)
{
    (void)path;memset(st,0,sizeof(*st));st->f_bsize=4096;st->f_frsize=4096;
    st->f_namemax=255;st->f_flag=ST_RDONLY|ST_NOSUID;return 0;
}
static void *ro_init(struct fuse_conn_info *conn,struct fuse_config *cfg)
{
    (void)conn;cfg->use_ino=1;cfg->entry_timeout=60;cfg->attr_timeout=60;cfg->negative_timeout=0;
    fprintf(stderr,"OK mount ready (read-only, foreground)\n");return view();
}
/* Explicit EROFS even when handlers are invoked without the kernel RO guard. */
static int deny_mknod(const char *p,mode_t m,dev_t d) {(void)p;(void)m;(void)d;return -EROFS;}
static int deny_mkdir(const char *p,mode_t m) {(void)p;(void)m;return -EROFS;}
static int deny_path(const char *p) {(void)p;return -EROFS;}
static int deny_link(const char *a,const char *b) {(void)a;(void)b;return -EROFS;}
static int deny_rename(const char *a,const char *b,unsigned int f) {(void)a;(void)b;(void)f;return -EROFS;}
static int deny_chmod(const char *p,mode_t m,struct fuse_file_info *f) {(void)p;(void)m;(void)f;return -EROFS;}
static int deny_chown(const char *p,uid_t u,gid_t g,struct fuse_file_info *f) {(void)p;(void)u;(void)g;(void)f;return -EROFS;}
static int deny_truncate(const char *p,off_t n,struct fuse_file_info *f) {(void)p;(void)n;(void)f;return -EROFS;}
static int deny_create(const char *p,mode_t m,struct fuse_file_info *f) {(void)p;(void)m;(void)f;return -EROFS;}
static int deny_write(const char *p,const char *b,size_t n,off_t o,struct fuse_file_info *f)
{(void)p;(void)b;(void)n;(void)o;(void)f;return -EROFS;}
static int deny_setxattr(const char *p,const char *n,const char *v,size_t s,int f)
{(void)p;(void)n;(void)v;(void)s;(void)f;return -EROFS;}
static int deny_utimens(const char *p,const struct timespec ts[2],struct fuse_file_info *f)
{(void)p;(void)ts;(void)f;return -EROFS;}
static int deny_fallocate(const char *p,int m,off_t o,off_t n,struct fuse_file_info *f)
{(void)p;(void)m;(void)o;(void)n;(void)f;return -EROFS;}
static int no_xattr(const char *p,const char *n,char *v,size_t s) {(void)p;(void)n;(void)v;(void)s;return -ENODATA;}
static int no_xattrs(const char *p,char *v,size_t s) {(void)p;(void)v;(void)s;return 0;}
static int readlink_file(const char *p,char *b,size_t n) {(void)p;(void)b;(void)n;return -EINVAL;}
static int read_flush(const char *p,struct fuse_file_info *f) {(void)p;return f && find_file(view(),f->fh)?0:-EBADF;}
static int no_sync(const char *p,int d,struct fuse_file_info *f) {(void)p;(void)d;(void)f;return 0;}
static int close_dir(const char *p,struct fuse_file_info *f) {(void)p;(void)f;return 0;}
const struct fuse_operations *tgfs_fuse_operations(void)
{
    static const struct fuse_operations ops={
        .init=ro_init,.getattr=ro_getattr,.open=ro_open,.read=ro_read,.release=ro_release,
        .opendir=ro_opendir,.readdir=ro_readdir,.releasedir=close_dir,.access=ro_access,.statfs=ro_statfs,
        .mknod=deny_mknod,.mkdir=deny_mkdir,.unlink=deny_path,.rmdir=deny_path,.symlink=deny_link,
        .rename=deny_rename,.link=deny_link,.chmod=deny_chmod,.chown=deny_chown,.truncate=deny_truncate,
        .create=deny_create,.write=deny_write,.setxattr=deny_setxattr,.removexattr=deny_link,
        .utimens=deny_utimens,.fallocate=deny_fallocate,.getxattr=no_xattr,.listxattr=no_xattrs,
        .readlink=readlink_file,.flush=read_flush,.fsync=no_sync,.fsyncdir=no_sync
    };
    return &ops;
}
