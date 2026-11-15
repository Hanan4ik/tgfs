#ifndef TGFS_REMOTE_H
#define TGFS_REMOTE_H
#include "tgfs.h"
typedef struct tgfs_remote_session tgfs_remote_session;
typedef struct {
    const char *capture,*token_file,*proxy,*cache_dir;
    unsigned timeout;
    size_t cache_limit;
    int offline,refresh,no_cache;
} tgfs_remote_options;
int tgfs_remote_open(const tgfs_remote_options *options,tgfs_remote_session **out,tgfs_error *error);
tgfs *tgfs_remote_fs(tgfs_remote_session *session);
void tgfs_remote_report(tgfs_remote_session *session,FILE *stream);
void tgfs_remote_close(tgfs_remote_session *session);
#endif
