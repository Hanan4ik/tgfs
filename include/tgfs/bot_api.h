#ifndef TGFS_BOT_API_H
#define TGFS_BOT_API_H

#include "cJSON.h"
#include <stddef.h>
#include <stdio.h>

typedef struct tgfs_bot tgfs_bot;
typedef enum {
    TGFS_BOT_OK, TGFS_BOT_LOCAL, TGFS_BOT_NETWORK, TGFS_BOT_HTTP,
    TGFS_BOT_API, TGFS_BOT_FORMAT, TGFS_BOT_LIMIT
} tgfs_bot_status;
typedef struct {
    tgfs_bot_status kind;
    long http_status;
    int api_code;
    unsigned retry_after;
    int message_unforwardable; /* Exact Bot API 400: message cannot be forwarded. */
    int message_missing; /* Exact Bot API 400: message to forward not found. */
    int outcome_unknown; /* A mutating request may have succeeded; never retry blindly. */
    char message[160];   /* Controlled text only: no tokens, URLs or raw server bodies. */
} tgfs_bot_error;

/* One synchronous client, owned by one thread. timeout_seconds: 1..300.
 * Token: owner-only regular file, no symlinks, decimal bot ID : opaque secret.
 * Production endpoint is fixed to https://api.telegram.org. */
int tgfs_bot_open(const char *token_file, unsigned timeout_seconds,
                  tgfs_bot **out, tgfs_bot_error *error);
/* NULL: environment; empty: direct; explicit URL overrides NO_PROXY.
 * Supported schemes: http, https, socks5, socks5h. No URL credentials. */
int tgfs_bot_set_proxy(tgfs_bot *bot, const char *url, tgfs_bot_error *error);
void tgfs_bot_close(tgfs_bot *bot);

/* Allowed JSON methods: getMe/getChat/getFile/sendMessage/forwardMessage/
 * editMessageText/editMessageCaption/deleteMessage. params is borrowed, result is owned by
 * caller (cJSON_Delete); result is NULL on failure. No automatic retries. */
int tgfs_bot_call(tgfs_bot *bot, const char *method, const cJSON *params,
                  cJSON **result, tgfs_bot_error *error);
int tgfs_bot_send_document(tgfs_bot *bot, const char *chat_id, const char *filename,
                           const void *bytes, size_t length, const char *caption,
                           cJSON **result, tgfs_bot_error *error);
/* file_path must be the relative path returned by getFile, not a URL.
 * Downloads at most 20 MiB, returns a buffer to free(), NULL on failure. */
int tgfs_bot_download(tgfs_bot *bot, const char *file_path,
                      unsigned char **bytes, size_t *length, tgfs_bot_error *error);

/* Safe conversions for numeric Telegram response fields (up to 52 bits). */
int tgfs_bot_integer(const cJSON *object, const char *key, long long *out);
int tgfs_bot_channel_id(const char *text, long long *out);
void tgfs_probe_help(FILE *stream);
int tgfs_probe_main(int argc, char **argv);

#endif
