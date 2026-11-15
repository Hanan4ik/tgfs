#ifndef TGFS_FUSE_BACKEND_H
#define TGFS_FUSE_BACKEND_H
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "tgfs/tgfs.h"
typedef struct tgfs_fuse_view tgfs_fuse_view;
tgfs_fuse_view *tgfs_fuse_view_new(tgfs *fs,tgfs_error *error);
void tgfs_fuse_view_close(tgfs_fuse_view *view);
const struct fuse_operations *tgfs_fuse_operations(void);
#ifdef TGFS_TESTING
void tgfs_fuse_test_context(tgfs_fuse_view *view);
#endif
#endif
