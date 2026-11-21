#ifndef TGFS_INTERNAL_H
#define TGFS_INTERNAL_H
#include "tgfs/tgfs.h"
#include "cJSON.h"
#include "tgfs/bot_api.h"
#include <stdint.h>

#define TGFS_BOT_MAX_EXACT_ID 4503599627370495LL
#define TGFS_MAX_JSON (8U * 1024U * 1024U)
#define TGFS_MAX_CHUNK 20971520U
#define TGFS_MAX_RECORDS 65536

int fail_code(tgfs_error *e, int code, const char *format, ...);
int fail(tgfs_error *e, const char *format, ...);
int tgfs_duplicate_groups(tgfs *fs, cJSON *groups, cJSON *chunk_refs, tgfs_error *e);
int stage_intent_at(int dirfd, const char *file, const char *raw, tgfs_error *e);
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

struct tgfs_bot;
int directory_target(tgfs *fs, const char *path, const char *name, char object[37], char parent[37], tgfs_error *e);
int remote_directory_target(struct tgfs_bot *bot, const cJSON *journal, const char *path,
                            const char *name, char object[37], char parent[37], tgfs_error *e);

int replacement_target(tgfs *fs, const char *path, const char *name, char object[37], char parent[37],
                       char version[37], uint64_t *commit_id, tgfs_error *e);
int remote_replacement_target(struct tgfs_bot *bot, const cJSON *journal, const char *path,
                              const char *name, char object[37], char parent[37], char version[37],
                              uint64_t *commit_id, tgfs_error *e);
cJSON *directory_observed_adds(const tgfs_directory_batch *batches, size_t count, const char *directory,
                               const char *name, const char *object, tgfs_error *e);
int unlink_target(tgfs *fs, const char *path, const char *name, char object[37], char parent[37],
                  char grandparent[37], cJSON **observed, tgfs_error *e);
int remote_unlink_target(struct tgfs_bot *bot, const cJSON *journal, const char *path, const char *name,
                         char object[37], char parent[37], char grandparent[37], cJSON **observed, tgfs_error *e);
int repair_backlinks(struct tgfs_bot *bot, const cJSON *journal, const cJSON *intent, cJSON *report, int log_fd, tgfs_error *e);
int resume_directory(struct tgfs_bot *bot, tgfs *fs, const cJSON *journal, const cJSON *intent, cJSON *report, int log_fd, tgfs_error *e);
int resume_unlink(struct tgfs_bot *bot, tgfs *fs, const cJSON *journal, const cJSON *intent, cJSON *report, int log_fd, tgfs_error *e);
int finalize_put(struct tgfs_bot *bot, const char *state, const cJSON *journal, const cJSON *intent, cJSON *report, int logfd, tgfs_error *e);
int finalize_metadata(struct tgfs_bot *bot, const char *state, const cJSON *journal, const cJSON *intent, cJSON *report, int logfd, tgfs_error *e);
int upload_log_event(int fd, const char *event, const char *id, tgfs_error *e);
int upload_attempts(int fd, const char *operation, unsigned char attempted[4096], int *commit_attempted, tgfs_error *e);
int upload_request_event(int fd, const char *event, const char *id, const char *operation, tgfs_error *e);
int upload_rejected(const tgfs_bot_error *b);
int upload_rejection(int fd, const char *event, const char *id, const char *operation, const tgfs_bot_error *b, tgfs_error *e);
int verify_upload_part(struct tgfs_bot *bot, const cJSON *journal, const cJSON *record, int fd, tgfs_error *e);
int resume_upload(struct tgfs_bot *bot, tgfs *fs, const char *state, const cJSON *journal, const cJSON *intent, cJSON *report, int fd, tgfs_error *e);
#endif
