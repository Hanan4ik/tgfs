#ifndef TGFS_H
#define TGFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Local snapshot adapter. No network, writes, or implicit formatting. */
typedef struct tgfs tgfs;
typedef struct { char message[256]; int code; } tgfs_error;
typedef int (*tgfs_entry_fn)(const char *name, const char *object_id, void *arg);

int tgfs_open_snapshot(const char *directory, tgfs **out, tgfs_error *error);
/* Optional CHUNK loader: returns malloc-owned bytes, NULL on failure.
 * Metadata is still read from the snapshot. Called only for selected parts. */
typedef unsigned char *(*tgfs_chunk_loader)(uint64_t message_id, size_t *length,
                                            void *context, tgfs_error *error);
int tgfs_open_snapshot_loader(const char *directory, tgfs_chunk_loader loader,
                             void *context, tgfs **out, tgfs_error *error);
void tgfs_close(tgfs *fs);
/* Immutable view metadata. Inodes are assigned locally within this snapshot. */
typedef struct { uint64_t inode, size, links; int directory; } tgfs_attr;
typedef struct tgfs_file tgfs_file;
int tgfs_getattr(tgfs *fs, const char *path, tgfs_attr *attr, tgfs_error *error);
int tgfs_object_attr(tgfs *fs, const char *object_id, tgfs_attr *attr, tgfs_error *error);
/* Single-threaded handles/fs. fs must outlive every open file. A handle pins its selected FILE_COMMIT. */
int tgfs_file_open(tgfs *fs, const char *path, tgfs_file **out, tgfs_error *error);
void tgfs_file_close(tgfs_file *file);
void tgfs_file_attr(const tgfs_file *file, tgfs_attr *attr);
/* Only overlapping parts are loaded; each is fully verified before copying.
 * Failed calls set *length=0. Buffer contents must be ignored on failure. */
int tgfs_file_read(tgfs_file *file, uint64_t offset, void *buffer, size_t capacity,
                   size_t *length, tgfs_error *error);
int tgfs_list(tgfs *fs, const char *path, tgfs_entry_fn emit, void *arg,
              tgfs_error *error);
/* Streams one selected immutable version, verifying each part before output.
 * A late error can leave a verified prefix in output; callers check status. */
int tgfs_cat(tgfs *fs, const char *path, FILE *output, tgfs_error *error);

#endif
