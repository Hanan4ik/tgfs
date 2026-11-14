#ifndef TGFS_H
#define TGFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Local snapshot adapter. No network, writes, or implicit formatting. */
typedef struct tgfs tgfs;
typedef struct { char message[256]; } tgfs_error;
typedef int (*tgfs_entry_fn)(const char *name, const char *object_id, void *arg);

int tgfs_open_snapshot(const char *directory, tgfs **out, tgfs_error *error);
/* Optional CHUNK loader: returns malloc-owned bytes, NULL on failure.
 * Metadata is still read from the snapshot. Called only for selected parts. */
typedef unsigned char *(*tgfs_chunk_loader)(uint64_t message_id, size_t *length,
                                            void *context, tgfs_error *error);
int tgfs_open_snapshot_loader(const char *directory, tgfs_chunk_loader loader,
                             void *context, tgfs **out, tgfs_error *error);
void tgfs_close(tgfs *fs);
int tgfs_list(tgfs *fs, const char *path, tgfs_entry_fn emit, void *arg,
              tgfs_error *error);
/* Streams one selected immutable version, verifying each part before output.
 * A late error can leave a verified prefix in output; callers check status. */
int tgfs_cat(tgfs *fs, const char *path, FILE *output, tgfs_error *error);

#endif
