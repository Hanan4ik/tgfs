#ifndef TGFS_INTERNAL_H
#define TGFS_INTERNAL_H
#include "tgfs/tgfs.h"
#include "cJSON.h"
#include <stdint.h>

#define TGFS_MAX_JSON (8U * 1024U * 1024U)
#define TGFS_MAX_CHUNK 20971520U
#define TGFS_MAX_RECORDS 65536

int fail(tgfs_error *e, const char *format, ...);
cJSON *json_load_at(int dirfd, const char *file, tgfs_error *e);
cJSON *json_parse_bytes(const unsigned char *raw, size_t length, tgfs_error *e);
unsigned char *bytes_load_at(int dirfd, const char *file, size_t limit,
                             size_t *length, tgfs_error *e);
const char *json_string(const cJSON *object, const char *key);
int decimal(const char *s, uint64_t *out);
int json_decimal(const cJSON *o, const char *key, uint64_t *out);
int json_count(const cJSON *o, const char *key, size_t *out);
int uuid_valid(const char *s);
int hash_valid(const char *s);
int name_valid(const char *s);
int json_semantic_equal(const cJSON *a, const cJSON *b);

/* Batches must be sorted by source message ID. Bodies are borrowed. */
typedef struct {
    uint64_t message_id;
    const char *object_id;
    const cJSON *body;
} tgfs_directory_batch;
cJSON *directory_merge(const tgfs_directory_batch *batches, size_t count, tgfs_error *e);
int directory_operations_valid(const cJSON *body, tgfs_error *e);

#endif
