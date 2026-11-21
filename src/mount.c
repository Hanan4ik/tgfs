#include "fuse_backend.h"
#include "tgfs/remote.h"
#include "tgfs/scan.h"
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int same(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static char *mount_directory(const char *path,tgfs_error *e)
{
    char *normalized=strdup(path);
    if(!normalized) {fail_code(e,ENOMEM,"out of memory");return NULL;}
    size_t n=strlen(normalized);
    while(n>1 && normalized[n-1]=='/') normalized[--n]=0;
    struct stat st;
    if(lstat(normalized,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid()) {
        free(normalized);fail(e,"mountpoint must be an existing owned directory, not a symlink");return NULL;
    }
    char *absolute=realpath(normalized,NULL);free(normalized);
    if(!absolute) {fail(e,"cannot resolve mountpoint");return NULL;}
    DIR *dir=opendir(absolute);if(!dir) {free(absolute);fail(e,"cannot inspect mountpoint");return NULL;}
    struct dirent *entry;int empty=1;errno=0;
    while((entry=readdir(dir))) if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) {empty=0;break;}
    int read_error=!entry && errno;
    closedir(dir);
    if(read_error) {free(absolute);fail(e,"cannot enumerate mountpoint");return NULL;}
    if(!empty) {free(absolute);fail(e,"mountpoint must be empty");return NULL;}
    return absolute;
}
void tgfs_mount_help(FILE *stream)
{
    fputs("usage: tgfs mount --mountpoint DIRECTORY {--snapshot DIRECTORY | --capture DIRECTORY --token-file PATH} [options]\n"
          "Mount one immutable captured view read-only; stays in foreground (Ctrl-C to unmount).\n"
          "  --mountpoint DIRECTORY   Existing empty directory owned by this user.\n"
          "  --snapshot DIRECTORY     Local fixture, no Telegram token needed.\n"
          "  --capture DIRECTORY      Completed Telegram scan journal.\n"
          "  --token-file PATH        Token for the bot used by scan (capture mode).\n"
          "  --proxy URL | --direct   Capture mode transport routing.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --cache-dir DIRECTORY    Private SQLite cache, as in read.\n"
          "  --cache-limit-mib N      1..4096, default 256.\n"
          "  --open-files-dir DIR     New private directory for captured open-file references.\n"
          "  --offline                Capture mode uses cached metadata/parts only.\n"
          "  --refresh-cache          Refresh metadata; fetched parts replace cache entries.\n"
          "  --no-cache               Temporary metadata and one RAM chunk only.\n"
          "  --cache-stats            Report counters before/after the mount session.\n"
          "  -h, --help               Show help without accessing FUSE/Telegram.\n"
          "Requires libfuse3 and /dev/fuse. Single-threaded, no auto-refresh or write operations.\n",stream);
}
int tgfs_mount_main(int argc,char **argv)
{
    const char *point=NULL,*snapshot=NULL,*capture=NULL,*token=NULL,*proxy=NULL,*cache_dir=NULL,*timeout_text=NULL,*limit_text=NULL,*open_files=NULL;
    int offline=0,refresh=0,no_cache=0,stats=0;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_mount_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--offline")) {if(offline) goto usage;offline=1;continue;}
        if(same(argv[i],"--refresh-cache")) {if(refresh) goto usage;refresh=1;continue;}
        if(same(argv[i],"--no-cache")) {if(no_cache) goto usage;no_cache=1;continue;}
        if(same(argv[i],"--cache-stats")) {if(stats) goto usage;stats=1;continue;}
        if(same(argv[i],"--mountpoint")) slot=&point;
        else if(same(argv[i],"--open-files-dir")) slot=&open_files;
        else if(same(argv[i],"--snapshot")) slot=&snapshot;
        else if(same(argv[i],"--capture")) slot=&capture;
        else if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--cache-dir")) slot=&cache_dir;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        else if(same(argv[i],"--cache-limit-mib")) slot=&limit_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    uint64_t timeout=60,limit=256;
    if(!point || (!!snapshot==!!capture) || (capture && !token) ||
        (snapshot && (open_files || token || proxy || cache_dir || timeout_text || limit_text || offline || refresh || no_cache || stats)) ||
        (offline && refresh) || (no_cache && (offline || refresh || cache_dir || limit_text)) ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300)) ||
        (limit_text && (decimal(limit_text,&limit) || !limit || limit>4096 || limit>SIZE_MAX/(1024U*1024U)))) goto usage;
    tgfs_error e={0};int rc=1;tgfs *fs=NULL;tgfs_remote_session *session=NULL;tgfs_fuse_view *v=NULL;
    char *absolute=mount_directory(point,&e);if(!absolute) goto done;
    if(access("/dev/fuse",R_OK|W_OK)) {fail(&e,"/dev/fuse is unavailable or inaccessible; mount requires a FUSE-enabled host");goto done;}
    if(snapshot) {if(tgfs_open_snapshot(snapshot,&fs,&e)) goto done;}
    else {
        tgfs_remote_options o={.capture=capture,.token_file=token,.proxy=proxy,.cache_dir=cache_dir,
            .timeout=(unsigned)timeout,.cache_limit=(size_t)limit*1024U*1024U,.offline=offline,.refresh=refresh,.no_cache=no_cache};
        if(tgfs_remote_open(&o,&session,&e)) goto done;
        fs=tgfs_remote_fs(session);
    }
    v=tgfs_fuse_view_new(fs,&e);if(!v) goto done;
    if(open_files) {
        if(tgfs_fuse_track_open_files(v,open_files,tgfs_remote_journal(session),&e)) goto done;
        /* The registry must stay outside the directory that FUSE will cover. */
        char *checked=mount_directory(absolute,&e);
        if(!checked) goto done;
        free(checked);
    }
    if(stats && session) tgfs_remote_report(session,stderr);
    char *fuse_argv[]={"tgfs","-f","-s","-o","ro,default_permissions,nodev,nosuid",absolute,NULL};
    rc=fuse_main(6,fuse_argv,tgfs_fuse_operations(),v) ? 1 : 0;
    if(rc) fail(&e,"FUSE mount or session failed; see libfuse diagnostics above");
done:
    if(rc) fprintf(stderr,"FAIL mount: %s\n",e.message);
    tgfs_fuse_view_close(v);
    if(stats && session) tgfs_remote_report(session,stderr);
    if(session) tgfs_remote_close(session);else tgfs_close(fs);
    free(absolute);return rc;
usage:
    tgfs_mount_help(stderr);return 2;
}
