#include "tgfs/bot_api.h"
#include "internal.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

static int verification(tgfs_bot_error *e, const char *message)
{
    memset(e, 0, sizeof(*e));
    e->kind = TGFS_BOT_FORMAT;
    snprintf(e->message, sizeof(e->message), "%s", message);
    return -1;
}

static cJSON *one_string(const char *key, const char *value)
{
    cJSON *p = cJSON_CreateObject();
    if (!p || !cJSON_AddStringToObject(p, key, value)) { cJSON_Delete(p); return NULL; }
    return p;
}

static int request(tgfs_bot *bot, const char *method, cJSON *params,
                   cJSON **result, tgfs_bot_error *e)
{
    cJSON_Delete(*result);
    *result = NULL;
    int rc = tgfs_bot_call(bot, method, params, result, e);
    cJSON_Delete(params);
    return rc;
}

static int channel_matches(const cJSON *o, long long expected)
{
    long long id;
    const char *type = json_string(o, "type");
    return !tgfs_bot_integer(o, "id", &id) && id == expected && type && !strcmp(type, "channel");
}

static int message_matches(const cJSON *o, long long chat, const char *filename,
                           const char *caption, size_t length, long long *mid)
{
    if (tgfs_bot_integer(o, "message_id", mid) || *mid <= 0 ||
        !channel_matches(cJSON_GetObjectItemCaseSensitive(o, "chat"), chat)) return 0;
    const cJSON *doc = cJSON_GetObjectItemCaseSensitive(o, "document");
    const char *name = json_string(doc, "file_name"), *fid = json_string(doc, "file_id");
    const char *received_caption = json_string(o, "caption");
    long long size;
    return name && !strcmp(name, filename) && fid && *fid && received_caption &&
        !strcmp(received_caption, caption) && !tgfs_bot_integer(doc, "file_size", &size) &&
        size == (long long)length;
}

static int sha256(const void *bytes, size_t length, char hex[65])
{
    unsigned char hash[32]; unsigned count;
    if (!EVP_Digest(bytes, length, hash, &count, EVP_sha256(), NULL) || count != 32) return -1;
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return 0;
}

static int probe(tgfs_bot *bot, const char *storage_text, const char *service_text,
                  long long storage, long long service)
{
    tgfs_bot_error e = {0};
    cJSON *result = NULL;
    unsigned char *downloaded = NULL;
    size_t downloaded_size = 0;
    const char *stage = "getMe";
    long long bot_id, source_id = 0, copy_id = 0, edited_id;
    int rc = 1;
    unsigned char payload[4096];
    char expected_hash[65], actual_hash[65], caption[256], edited[320];
    const char *filename = "tgfs-probe.bin.0";
    if (RAND_bytes(payload, (int)sizeof(payload)) != 1 || sha256(payload, sizeof(payload), expected_hash)) {
        verification(&e, "cannot create probe bytes"); goto done;
    }
    snprintf(caption, sizeof(caption),
             "{\"magic\":\"TGFS_PROBE\",\"sha256\":\"%s\",\"next_message\":null}", expected_hash);
    snprintf(edited, sizeof(edited),
             "{\"magic\":\"TGFS_PROBE\",\"sha256\":\"%s\",\"edited\":true,\"next_message\":null}", expected_hash);
    if (request(bot, "getMe", cJSON_CreateObject(), &result, &e)) goto done;
    if (tgfs_bot_integer(result, "id", &bot_id) || bot_id <= 0 ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "is_bot"))) {
        verification(&e, "unexpected bot identity"); goto done;
    }
    printf("OK getMe bot_id=%lld\n", bot_id);
    fflush(stdout);
    stage = "getChat storage";
    if (request(bot, "getChat", one_string("chat_id", storage_text), &result, &e)) goto done;
    if (!channel_matches(result, storage)) { verification(&e, "storage ID does not identify the requested channel"); goto done; }
    printf("OK getChat storage=%lld\n", storage);
    stage = "getChat service";
    if (request(bot, "getChat", one_string("chat_id", service_text), &result, &e)) goto done;
    if (!channel_matches(result, service)) { verification(&e, "service ID does not identify the requested channel"); goto done; }
    printf("OK getChat service=%lld\n", service);
    fflush(stdout);

    stage = "sendDocument";
    cJSON_Delete(result); result = NULL;
    if (tgfs_bot_send_document(bot, storage_text, filename, payload, sizeof(payload), caption, &result, &e)) goto done;
    if (!message_matches(result, storage, filename, caption, sizeof(payload), &source_id)) {
        verification(&e, "unexpected uploaded document metadata"); e.outcome_unknown = 1; goto done;
    }
    printf("OK sendDocument source_id=%lld filename=%s bytes=%zu\n", source_id, filename, sizeof(payload));
    fflush(stdout);

    stage = "editMessageCaption";
    cJSON *params = one_string("chat_id", storage_text);
    if (!params || !cJSON_AddNumberToObject(params, "message_id", (double)source_id) ||
        !cJSON_AddStringToObject(params, "caption", edited)) {
        cJSON_Delete(params); verification(&e, "cannot build edit request"); goto done;
    }
    if (request(bot, "editMessageCaption", params, &result, &e)) goto done;
    if (!message_matches(result, storage, filename, edited, sizeof(payload), &edited_id) || edited_id != source_id) {
        verification(&e, "caption edit did not return the requested document"); e.outcome_unknown = 1; goto done;
    }
    printf("OK editMessageCaption source_id=%lld\n", source_id);
    fflush(stdout);

    stage = "forwardMessage";
    params = one_string("chat_id", service_text);
    if (!params || !cJSON_AddStringToObject(params, "from_chat_id", storage_text) ||
        !cJSON_AddNumberToObject(params, "message_id", (double)source_id) ||
        !cJSON_AddBoolToObject(params, "disable_notification", 1)) {
        cJSON_Delete(params); verification(&e, "cannot build forward request"); goto done;
    }
    if (request(bot, "forwardMessage", params, &result, &e)) goto done;
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(result, "forward_origin");
    const char *origin_type = json_string(origin, "type");
    long long origin_id;
    if (!message_matches(result, service, filename, edited, sizeof(payload), &copy_id) ||
        !origin_type || strcmp(origin_type, "channel") ||
        !channel_matches(cJSON_GetObjectItemCaseSensitive(origin, "chat"), storage) ||
        tgfs_bot_integer(origin, "message_id", &origin_id) || origin_id != source_id) {
        verification(&e, "forward_origin or forwarded document does not match source"); goto done;
    }
    printf("OK forwardMessage copy_id=%lld source_id=%lld\n", copy_id, origin_id);
    fflush(stdout);
    const cJSON *document = cJSON_GetObjectItemCaseSensitive(result, "document");
    params = one_string("file_id", json_string(document, "file_id"));
    stage = "getFile";
    if (request(bot, "getFile", params, &result, &e)) goto done;
    const char *path = json_string(result, "file_path");
    long long advertised;
    if (!path || tgfs_bot_integer(result, "file_size", &advertised) || advertised != sizeof(payload)) {
        verification(&e, "unexpected getFile metadata"); goto done;
    }
    stage = "download";
    if (tgfs_bot_download(bot, path, &downloaded, &downloaded_size, &e)) goto done;
    if (downloaded_size != sizeof(payload) || sha256(downloaded, downloaded_size, actual_hash) ||
        strcmp(expected_hash, actual_hash) || memcmp(payload, downloaded, sizeof(payload))) {
        verification(&e, "downloaded size, bytes or SHA-256 mismatch"); goto done;
    }
    printf("OK download bytes=%zu sha256=%s\n", downloaded_size, actual_hash);
    printf("PASS transport probe; test messages remain: storage=%lld/%lld service=%lld/%lld\n",
           storage, source_id, service, copy_id);
    rc = 0;
done:
    if (rc) {
        fprintf(stderr, "FAIL %s: %s [kind=%d http=%ld api=%d retry_after=%u outcome_unknown=%d]\n",
                stage, e.message, (int)e.kind, e.http_status, e.api_code, e.retry_after, e.outcome_unknown);
        if (source_id > 0) fprintf(stderr, "created storage message_id=%lld\n", source_id);
        if (copy_id > 0) fprintf(stderr, "created service message_id=%lld\n", copy_id);
    }
    cJSON_Delete(result); free(downloaded);
    if (fflush(stdout)) rc = 1;
    return rc;
}

int tgfs_probe_main(int argc, char **argv)
{
    const char *token_file = NULL, *storage_text = NULL, *service_text = NULL, *timeout_text = NULL;
    const char *proxy = NULL;
    unsigned timeout = 60;
    long long storage, service;
    for (int i = 2; i < argc; i += 2) {
        if (!strcmp(argv[i], "--direct")) {
            if (proxy) goto usage;
            proxy = ""; i--; continue;
        }
        if (i + 1 >= argc) goto usage;
        const char **slot;
        if (!strcmp(argv[i], "--token-file")) slot = &token_file;
        else if (!strcmp(argv[i], "--storage-chat-id")) slot = &storage_text;
        else if (!strcmp(argv[i], "--service-chat-id")) slot = &service_text;
        else if (!strcmp(argv[i], "--proxy")) slot = &proxy;
        else if (!strcmp(argv[i], "--timeout-seconds")) slot = &timeout_text;
        else goto usage;
        if (*slot) goto usage;
        *slot = argv[i + 1];
    }
    if (!token_file || tgfs_bot_channel_id(storage_text, &storage) ||
        tgfs_bot_channel_id(service_text, &service) || storage == service) goto usage;
    if (timeout_text) {
        uint64_t value;
        if (decimal(timeout_text, &value) || value < 1 || value > 300) goto usage;
        timeout = (unsigned)value;
    }
    tgfs_bot *bot = NULL;
    tgfs_bot_error e;
    if (tgfs_bot_open(token_file, timeout, &bot, &e)) {
        fprintf(stderr, "FAIL token/client: %s\n", e.message); return 1;
    }
    if (tgfs_bot_set_proxy(bot, proxy, &e)) {
        fprintf(stderr, "FAIL proxy: %s\n", e.message);
        tgfs_bot_close(bot); return 1;
    }
    int rc = probe(bot, storage_text, service_text, storage, service);
    tgfs_bot_close(bot);
    return rc;
usage:
    fprintf(stderr, "usage: tgfs probe --token-file PATH --storage-chat-id -100... "
                    "--service-chat-id -100... [--timeout-seconds 60] [--proxy URL | --direct]\n");
    return 2;
}
