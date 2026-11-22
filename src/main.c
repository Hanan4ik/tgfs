#include "tgfs/tgfs.h"
#include "tgfs/bot_api.h"
#include "tgfs/scan.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int print_entry(const char *name, const char *object_id, void *arg)
{
    (void)object_id;
    return fprintf((FILE *)arg, "%s\n", name) < 0;
}

static void help(FILE *stream)
{
    fputs("TGFS - Telegram File System prototype\n\n"
          "usage: tgfs --snapshot DIRECTORY {ls|cat} /absolute/path\n"
          "       tgfs probe --token-file PATH --storage-chat-id ID --service-chat-id ID [options]\n"
          "       tgfs scan --token-file PATH --storage-chat-id ID --service-chat-id ID --output DIRECTORY [options]\n"
          "       tgfs read --capture DIRECTORY --token-file PATH [options] {ls|cat} /absolute/path\n"
          "       tgfs gc-plan --capture DIRECTORY --token-file PATH [options]\n"
          "       tgfs put --capture DIRECTORY --file PATH --name NAME --state-dir DIRECTORY --token-file PATH [options]\n"
          "       tgfs intent --state-dir DIRECTORY --capture DIRECTORY --token-file PATH [options]\n"
          "       tgfs unlink --capture DIRECTORY --path /absolute/file --state-dir DIRECTORY --token-file PATH [options]\n"
          "       tgfs mkdir --capture DIRECTORY --path /absolute/path --state-dir DIRECTORY --token-file PATH [options]\n"
          "       tgfs init --new-volume --token-file PATH --storage-chat-id ID --state-dir DIRECTORY [options]\n"
          "       tgfs mount --mountpoint DIRECTORY {--snapshot DIRECTORY | --capture DIRECTORY --token-file PATH} [options]\n"
          "       tgfs --help\n\n"
          "Global option before the command:\n"
          "  --volume-lock DIRECTORY  Use an existing private directory as a local volume lock.\n"
          "  --shared-volume-lock FILE  Use an existing private file on shared storage.\n"
          "                           gc-plan takes it exclusively; other commands share it.\n"
          "  lock-check {--shared | --exclusive} [--hold]  Test the selected lock; Enter releases it.\n\n"
          "Snapshot commands (local data, no Telegram connection):\n"
          "  --snapshot DIRECTORY     Read snapshot.json and documents from DIRECTORY.\n"
          "  ls /absolute/path        List immediate directory entries.\n"
          "  cat /absolute/path       Read a file and verify part/file SHA-256.\n\n"
          "  -h, --help               Show all available commands and options.\n\n"
          "mount provides a read-only captured view via FUSE; writes return EROFS.\n"
          "Exit codes: 0 success/help, 1 operation failed, 2 invalid arguments.\n\n", stream);
    tgfs_probe_help(stream);
    fputc('\n', stream);
    tgfs_scan_help(stream);
    fputc('\n', stream);
    tgfs_init_help(stream);
    fputc('\n',stream);tgfs_read_help(stream);
    fputc('\n',stream);tgfs_gc_plan_help(stream);
    fputc('\n',stream);tgfs_put_help(stream);
    fputc('\n',stream);tgfs_mount_help(stream);
    fputc('\n',stream);tgfs_mkdir_help(stream);
    fputc('\n',stream);tgfs_unlink_help(stream);
    fputc('\n',stream);tgfs_intent_help(stream);
}

static int dispatch(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        help(stdout); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "intent")) return tgfs_intent_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "unlink")) return tgfs_unlink_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "mkdir")) return tgfs_mkdir_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "mount")) return tgfs_mount_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "put")) return tgfs_put_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "gc-plan")) return tgfs_gc_plan_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "read")) return tgfs_read_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "init")) return tgfs_init_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "scan")) return tgfs_scan_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "probe")) return tgfs_probe_main(argc, argv);
    if (argc != 5 || strcmp(argv[1], "--snapshot") ||
        (strcmp(argv[3], "ls") && strcmp(argv[3], "cat"))) {
        help(stderr);
        return 2;
    }
    tgfs_error error = {0};
    tgfs *fs = NULL;
    if (tgfs_open_snapshot(argv[2], &fs, &error)) {
        fprintf(stderr, "tgfs: %s\n", error.message); return 1;
    }
    int result = !strcmp(argv[3], "ls")
        ? tgfs_list(fs, argv[4], print_entry, stdout, &error)
        : tgfs_cat(fs, argv[4], stdout, &error);
    if (!result && fflush(stdout)) { snprintf(error.message, sizeof(error.message), "cannot flush stdout"); result = -1; }
    if (result) fprintf(stderr, "tgfs: %s\n", error.message);
    tgfs_close(fs);
    return result ? 1 : 0;
}


static int lock_volume(const char *path,int exclusive,int shared_storage)
{
    int flags=shared_storage?O_RDWR|O_NONBLOCK:O_RDONLY|O_DIRECTORY;
    int fd=open(path,flags|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(fd<0 || fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&0077) ||
       (shared_storage && (!S_ISREG(st.st_mode) || st.st_nlink!=1))) {
        if(fd>=0) close(fd);
        fprintf(stderr,shared_storage?
                "tgfs: shared volume lock must be an existing private regular file owned by this user\n":
                "tgfs: volume lock must be an existing private directory owned by this user\n");
        return -1;
    }
    int result;
    do {result=flock(fd,(exclusive?LOCK_EX:LOCK_SH)|LOCK_NB);} while(result && errno==EINTR);
    if(result) {
        int error=errno;
        close(fd);
        if(error==EWOULDBLOCK)
            fprintf(stderr,"tgfs: volume lock is busy: %s\n",path);
        else
            fprintf(stderr,"tgfs: cannot acquire volume lock: %s\n",strerror(error));
        return -1;
    }
    return fd;
}

static void lock_check_help(FILE *stream)
{
    fputs("usage: tgfs --shared-volume-lock FILE lock-check {--shared | --exclusive} [--hold]\n"
          "       tgfs --volume-lock DIRECTORY lock-check {--shared | --exclusive} [--hold]\n"
          "With --hold, keep the lock until Enter or EOF.\n",stream);
}

int main(int argc,char **argv)
{
    if(argc<2) return dispatch(argc,argv);
    if(!strcmp(argv[1],"lock-check")) {
        int help_only=argc==3 && (!strcmp(argv[2],"--help") || !strcmp(argv[2],"-h"));
        lock_check_help(help_only?stdout:stderr);
        return help_only?0:2;
    }
    int shared_storage=!strcmp(argv[1],"--shared-volume-lock");
    if(!shared_storage && strcmp(argv[1],"--volume-lock")) return dispatch(argc,argv);
    if(argc<4 || !*argv[2]) {
        fputs("usage: tgfs {--volume-lock DIRECTORY | --shared-volume-lock FILE} COMMAND [args]\n",stderr);
        return 2;
    }
    const char *lock_path=argv[2];
    argv[2]=argv[0];
    argc-=2;argv+=2;
    int check=!strcmp(argv[1],"lock-check"),exclusive=!strcmp(argv[1],"gc-plan"),hold=0;
    const char *last=argv[argc-1];
    if(argc<=3 && (!strcmp(last,"--help") || !strcmp(last,"-h"))) {
        if(check) {lock_check_help(stdout);return 0;}
        return dispatch(argc,argv);
    }
    if(check) {
        if(argc<3 || argc>4 || (strcmp(argv[2],"--shared") && strcmp(argv[2],"--exclusive")) ||
           (argc==4 && strcmp(argv[3],"--hold"))) {
            lock_check_help(stderr);return 2;
        }
        exclusive=!strcmp(argv[2],"--exclusive");
        hold=argc==4;
    }
    int fd=lock_volume(lock_path,exclusive,shared_storage);
    if(fd<0) return 1;
    int result;
    if(check) {
        result=printf("OK volume lock %s\n",exclusive?"exclusive":"shared")<0 || fflush(stdout);
        if(!result && hold) {
            int c;
            do {c=getchar();} while(c!=EOF && c!='\n');
            result=ferror(stdin)!=0;
        }
    } else {
        result=dispatch(argc,argv);
    }
    close(fd);
    return result;
}
