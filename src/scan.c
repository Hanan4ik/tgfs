#include "tgfs/scan.h"
#include "tgfs/bot_api.h"
#include "internal.h"
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Capture a finite remote journal. It deliberately does not select DIRECTORY
 * generations or pretend that an inaccessible ID is a deleted message. */
typedef struct {
    tgfs_bot *bot;
    const char *storage, *service;
    long long storage_id, service_id;
    cJSON *copies, *cleanup_pending;
    tgfs_bot_error error;
    int copy_log;
} scanner;

static int equal(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int bad(scanner *s, const char *message)
{
    memset(&s->error, 0, sizeof(s->error));
    s->error.kind = TGFS_BOT_FORMAT;
    snprintf(s->error.message, sizeof(s->error.message), "%s", message);
    return -1;
}
static cJSON *params(const char *chat)
{
    cJSON *p = cJSON_CreateObject();
    if (!p || !cJSON_AddStringToObject(p, "chat_id", chat)) { cJSON_Delete(p); return NULL; }
    return p;
}
static int call(scanner *s, const char *method, cJSON *p, cJSON **out)
{
    int rc = tgfs_bot_call(s->bot, method, p, out, &s->error);
    cJSON_Delete(p);
    return rc;
}
static int chat_is(const cJSON *message, long long expected)
{
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
    long long id;
    return !tgfs_bot_integer(chat, "id", &id) && id == expected && equal(json_string(chat, "type"), "channel");
}
static int check_chat(scanner *s, const char *chat, long long expected, cJSON **out)
{
    if (call(s, "getChat", params(chat), out)) return -1;
    long long id;
    if (tgfs_bot_integer(*out, "id", &id) || id != expected || !equal(json_string(*out, "type"), "channel"))
        return bad(s, "getChat did not return the requested channel");
    return 0;
}
static int add_id(cJSON *array, long long id)
{
    char text[32]; snprintf(text, sizeof(text), "%lld", id);
    cJSON *v = cJSON_CreateString(text);
    if (!v || !cJSON_AddItemToArray(array, v)) { cJSON_Delete(v); return -1; }
    return 0;
}
static int forward(scanner *s, long long id, cJSON **out)
{
    cJSON *p = params(s->service);
    if (!p || !cJSON_AddStringToObject(p, "from_chat_id", s->storage) ||
        !cJSON_AddNumberToObject(p, "message_id", (double)id) ||
        !cJSON_AddBoolToObject(p, "disable_notification", 1)) {
        cJSON_Delete(p); return bad(s, "cannot allocate forward request");
    }
    if (call(s, "forwardMessage", p, out)) return -1;
    long long copy, original;
    if (!chat_is(*out, s->service_id) || tgfs_bot_integer(*out, "message_id", &copy) || copy <= 0)
        return bad(s, "forward returned invalid destination message");
    if (add_id(s->copies, copy)) return bad(s, "cannot save service copy ID");
    char line[32]; int length = snprintf(line, sizeof(line), "%lld\n", copy);
    if (write(s->copy_log, line, (size_t)length) != length || fsync(s->copy_log)) {
        fprintf(stderr, "scan: service copy message_id=%lld could not be logged\n", copy);
        return bad(s, "cannot persist service copy ID");
    }
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(*out, "forward_origin");
    if (!equal(json_string(origin, "type"), "channel") || !chat_is(origin, s->storage_id) ||
        tgfs_bot_integer(origin, "message_id", &original) || original != id)
        return bad(s, "forward_origin does not match requested source");
    return 0;
}
static cJSON *record(scanner *s, const cJSON *message)
{
    const char *raw = json_string(message, "caption");
    if (!raw) raw = json_string(message, "text");
    if (!raw) return NULL;
    tgfs_error error;
    cJSON *r = json_parse_bytes((const unsigned char *)raw, strlen(raw), &error);
    if (!r) {
        if (strstr(raw, "TGFS")) bad(s, "malformed possible TGFS record");
        return NULL;
    }
    if (!equal(json_string(r, "magic"), "TGFS")) { cJSON_Delete(r); return NULL; }
    size_t version;
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(r, "next_message");
    uint64_t next_id;
    if (json_count(r, "format_version", &version) || version != 1 || !json_string(r, "type") ||
        !(cJSON_IsNull(next) || (cJSON_IsString(next) && !decimal(next->valuestring, &next_id) &&
                              next_id > 0 && next_id <= 4503599627370495ULL))) {
        cJSON_Delete(r); bad(s, "invalid TGFS envelope or unsupported version"); return NULL;
    }
    return r;
}
static int header(scanner *s, const cJSON *message, long long requested, cJSON **out, long long *start)
{
    if (!json_string(message, "text") || cJSON_HasObjectItem(message, "document") ||
        !chat_is(message, s->storage_id) || tgfs_bot_integer(message, "message_id", start) || *start <= 0 ||
        (requested && *start != requested)) return bad(s, "invalid SUPERBLOCK source message");
    *out = record(s, message);
    uint64_t declared;
    if (!*out || !equal(json_string(*out, "type"), "SUPERBLOCK") ||
        !uuid_valid(json_string(*out, "root_object_id")) || json_decimal(*out, "start_id", &declared) ||
        declared != (uint64_t)*start) return bad(s, "valid SUPERBLOCK required; scan does not initialize a channel");
    return 0;
}
static int save_json(int dirfd, const char *name, const cJSON *value)
{
    char *raw = cJSON_PrintUnformatted(value);
    if (!raw) return -1;
    size_t size = strlen(raw), done = 0;
    if (size > TGFS_MAX_JSON) { free(raw); return -1; }
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { free(raw); return -1; }
    int rc = 0;
    while (done < size) {
        ssize_t n = write(fd, raw + done, size - done);
        if (n <= 0) { rc = -1; break; }
        done += (size_t)n;
    }
    if (fsync(fd)) rc = -1;
    if (close(fd)) rc = -1;
    free(raw); return rc;
}
static void cleanup(scanner *s)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, s->copies) {
        uint64_t id;
        if (decimal(v->valuestring, &id)) continue;
        cJSON *p = params(s->service), *out = NULL;
        tgfs_bot_error saved = s->error;
        int failed = !p || !cJSON_AddNumberToObject(p, "message_id", (double)id);
        if (failed) cJSON_Delete(p);
        else failed = call(s, "deleteMessage", p, &out) || !cJSON_IsTrue(out);
        if (failed) {
            fprintf(stderr, "scan: cleanup pending service message_id=%llu\n", (unsigned long long)id);
            if (add_id(s->cleanup_pending, (long long)id))
                fprintf(stderr, "scan: cannot record pending cleanup ID\n");
        }
        cJSON_Delete(out); s->error = saved;
    }
    while (cJSON_GetArraySize(s->copies)) cJSON_DeleteItemFromArray(s->copies, 0);
}

static int scan(scanner *s, const char *directory, long long locator, unsigned budget)
{
    cJSON *chat = NULL, *message = NULL, *sb = NULL, *journal = NULL, *r = NULL;
    cJSON *records = NULL, *missing = NULL, *objects = NULL;
    long long start = 0, boundary = 0, last = 0, requested = 0;
    int dirfd = -1, rc = 1;
    unsigned found = 0;
    if (mkdir(directory, 0700)) { bad(s, "output directory must not already exist"); goto done; }
    dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) { bad(s, "cannot open output directory"); goto done; }
    s->copy_log = openat(dirfd, "service-copies.log", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->copy_log < 0 || fsync(dirfd)) { bad(s, "cannot create service copy log"); goto done; }
    if (check_chat(s, s->storage, s->storage_id, &chat)) goto done;
    if (locator) {
        requested = locator;
        if (forward(s, locator, &message)) goto done;
        /* Header() expects source addressing, never the copied message ID. */
        cJSON *source = cJSON_Duplicate(message, 1);
        if (!source) { bad(s, "cannot allocate header"); goto done; }
        cJSON_ReplaceItemInObjectCaseSensitive(source, "message_id", cJSON_CreateNumber((double)locator));
        /* getChat itself is the Chat object. */
        cJSON_ReplaceItemInObjectCaseSensitive(source, "chat", cJSON_Duplicate(chat, 1));
        int failed = header(s, source, locator, &sb, &start);
        cJSON_Delete(source);
        if (failed) goto done;
    } else if (header(s, cJSON_GetObjectItemCaseSensitive(chat, "pinned_message"), 0, &sb, &start)) goto done;
    cJSON_Delete(chat); chat = NULL;
    if (check_chat(s, s->service, s->service_id, &chat)) goto done;
    journal = cJSON_CreateObject(); records = cJSON_CreateArray(); missing = cJSON_CreateArray();
    objects = cJSON_CreateObject();
    if (!journal || !records || !missing || !objects) { bad(s, "cannot allocate journal"); goto done; }
    if (!cJSON_AddStringToObject(journal, "schema", "TGFS_JOURNAL_CAPTURE_V1") ||
        !cJSON_AddStringToObject(journal, "storage_chat_id", s->storage) ||
        !cJSON_AddStringToObject(journal, "service_chat_id", s->service) ||
        !cJSON_AddItemToObject(journal, "superblock", sb)) { bad(s, "cannot allocate journal header"); goto done; }
    sb = NULL;
    cJSON *p = params(s->storage);
    const char *marker = "{\"magic\":\"TGFS\",\"format_version\":1,\"type\":\"SCAN_MARKER\",\"next_message\":null}";
    if (!p || !cJSON_AddStringToObject(p, "text", marker) || !cJSON_AddBoolToObject(p, "disable_notification", 1)) {
        cJSON_Delete(p); bad(s, "cannot allocate marker request"); goto done;
    }
    cJSON_Delete(message); message = NULL;
    if (call(s, "sendMessage", p, &message)) goto done;
    if (!chat_is(message, s->storage_id) || tgfs_bot_integer(message, "message_id", &boundary) ||
        boundary <= start || !equal(json_string(message, "text"), marker)) {
        bad(s, "invalid SCAN_MARKER response"); s->error.outcome_unknown = 1; goto done;
    }
    printf("OK SCAN_MARKER source_id=%lld start_id=%lld\n", boundary, start); fflush(stdout);
    if ((unsigned long long)(boundary - start) > budget) { bad(s, "scan range exceeds --max-messages; marker remains"); goto done; }
    for (long long id = start + 1; id < boundary; id++) {
        cJSON_Delete(message); message = NULL;
        requested = id;
        if (forward(s, id, &message)) {
            if (s->error.kind != TGFS_BOT_API || s->error.http_status != 400 ||
                s->error.api_code != 400) goto done;
            /* Accepted channel contract: forwarding is enabled; 400 is a gap. */
            fprintf(stderr, "scan: source message_id=%lld skipped (forwardMessage 400)\n", id);
            if (add_id(missing, id)) { bad(s, "cannot record unavailable ID"); goto done; }
            memset(&s->error, 0, sizeof(s->error)); last = id; continue;
        }
        r = record(s, message);
        if (!r) { if (s->error.kind) goto done; last = id; continue; }
        const char *object = json_string(r, "object_id"), *type = json_string(r, "type");
        if (equal(type, "CHUNK") || equal(type, "DIRECTORY") || equal(type, "FILE_COMMIT")) {
            if (!uuid_valid(object)) { bad(s, "invalid object_id in object record"); goto done; }
            cJSON *refs = cJSON_GetObjectItemCaseSensitive(objects, object);
            if (!refs) {
                refs = cJSON_CreateArray();
                if (!refs || !cJSON_AddItemToObject(objects, object, refs)) {
                    cJSON_Delete(refs); bad(s, "cannot allocate object index"); goto done;
                }
            }
            if (add_id(refs, id)) { bad(s, "cannot index object record"); goto done; }
        }
        cJSON *entry = cJSON_CreateObject();
        char text[32]; snprintf(text, sizeof(text), "%lld", id);
        if (!entry || !cJSON_AddStringToObject(entry, "message_id", text) ||
            !cJSON_AddItemToObject(entry, "record", r)) {
            cJSON_Delete(entry); bad(s, "cannot allocate journal record"); goto done;
        }
        r = NULL;
        const cJSON *doc = cJSON_GetObjectItemCaseSensitive(message, "document");
        if (doc && !cJSON_AddItemToObject(entry, "document", cJSON_Duplicate(doc, 1))) {
            cJSON_Delete(entry); bad(s, "cannot save document metadata"); goto done;
        }
        if (!cJSON_AddItemToArray(records, entry)) {
            cJSON_Delete(entry); bad(s, "cannot append journal record"); goto done;
        }
        found++; last = id;
    }
    /* Recheck access; no errors are translated into absent messages. */
    cJSON_Delete(chat); chat = NULL;
    if (check_chat(s, s->storage, s->storage_id, &chat)) goto done;
    cleanup(s);
    char bound[32]; snprintf(bound, sizeof(bound), "%lld", boundary);
    if (!cJSON_AddStringToObject(journal, "boundary_id", bound) ||
        !cJSON_AddStringToObject(journal, "gap_policy", "forward_400_is_gap") ||
        !cJSON_AddBoolToObject(journal, "scan_completed", 1) ||
        !cJSON_AddBoolToObject(journal, "coverage_verified", cJSON_GetArraySize(missing) == 0) ||
        !cJSON_AddItemToObject(journal, "records", records)) { bad(s, "cannot finalize journal"); goto done; }
    records = NULL;
    if (!cJSON_AddItemToObject(journal, "object_messages", objects)) { bad(s, "cannot finalize object index"); goto done; }
    objects = NULL;
    if (!cJSON_AddItemToObject(journal, "unavailable_ids", missing)) { bad(s, "cannot finalize unavailable IDs"); goto done; }
    missing = NULL;
    if (!cJSON_AddItemToObject(journal, "cleanup_pending", s->cleanup_pending)) { bad(s, "cannot finalize cleanup"); goto done; }
    s->cleanup_pending = NULL;
    if (save_json(dirfd, "journal.tmp", journal) || renameat(dirfd, "journal.tmp", dirfd, "journal.json") || fsync(dirfd)) {
        bad(s, "cannot publish journal.json"); goto done;
    }
    printf("OK scan boundary=%lld records=%u unavailable=%d cleanup_pending=%d\n", boundary, found,
           cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(journal, "unavailable_ids")),
           cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(journal, "cleanup_pending")));
    rc = 0;
done:
    if (rc == 1) {
        fprintf(stderr, "FAIL scan requested_id=%lld last_examined_id=%lld marker_id=%lld: %s [http=%ld api=%d retry_after=%u outcome_unknown=%d]\n",
                requested, last, boundary, s->error.message, s->error.http_status, s->error.api_code,
                s->error.retry_after, s->error.outcome_unknown);
        /* On failure retain known copy IDs even if cleanup itself fails. */
        cleanup(s);
        if (dirfd >= 0) {
            if (save_json(dirfd, "cleanup_pending.json", s->cleanup_pending ? s->cleanup_pending :
                          cJSON_GetObjectItemCaseSensitive(journal, "cleanup_pending")))
                fprintf(stderr, "scan: could not persist cleanup queue\n");
        }
    }
    if (s->copy_log >= 0) { close(s->copy_log); s->copy_log = -1; }
    if (dirfd >= 0) close(dirfd);
    cJSON_Delete(chat); cJSON_Delete(message); cJSON_Delete(sb); cJSON_Delete(journal);
    cJSON_Delete(records); cJSON_Delete(missing); cJSON_Delete(objects); cJSON_Delete(r);
    return rc;
}

void tgfs_scan_help(FILE *stream)
{
    fputs("usage: tgfs scan --token-file PATH --storage-chat-id ID --service-chat-id ID --output DIRECTORY [options]\n"
          "Capture TGFS records between SUPERBLOCK and a new SCAN_MARKER into journal.json.\n"
          "Requires an initialized channel; no automatic init or DIRECTORY selection.\n"
          "  --output DIRECTORY       New private capture directory (must not exist).\n"
          "  --start-id ID            Explicit SUPERBLOCK locator; default: getChat pin.\n"
          "  --max-messages N         Maximum source ID span, 1..65536 (default 10000).\n"
          "  --token-file PATH        Owner-only bot token file.\n"
          "  --storage-chat-id ID     Full negative Bot API channel ID.\n"
          "  --service-chat-id ID     Distinct channel for forwarding.\n"
          "  --timeout-seconds N      1..300, default 60.\n"
          "  --proxy URL | --direct   Same routing options as probe.\n"
          "  -h, --help               Show help without network requests.\n"
          "Exit 0: scan completed (forwardMessage 400 counted as gaps); 1: failed; 2: arguments.\n", stream);
}
int tgfs_scan_main(int argc, char **argv)
{
    const char *token = NULL, *storage = NULL, *service = NULL, *output = NULL;
    const char *locator = NULL, *limit = NULL, *timeout_text = NULL, *proxy = NULL;
    if (argc == 3 && (equal(argv[2], "--help") || equal(argv[2], "-h"))) { tgfs_scan_help(stdout); return 0; }
    for (int i = 2; i < argc; i++) {
        const char **slot = NULL;
        if (equal(argv[i], "--direct")) { if (proxy) goto usage; proxy = ""; continue; }
        if (equal(argv[i], "--token-file")) slot = &token;
        else if (equal(argv[i], "--storage-chat-id")) slot = &storage;
        else if (equal(argv[i], "--service-chat-id")) slot = &service;
        else if (equal(argv[i], "--output")) slot = &output;
        else if (equal(argv[i], "--start-id")) slot = &locator;
        else if (equal(argv[i], "--max-messages")) slot = &limit;
        else if (equal(argv[i], "--timeout-seconds")) slot = &timeout_text;
        else if (equal(argv[i], "--proxy")) slot = &proxy;
        if (!slot || *slot || i + 1 >= argc) goto usage;
        *slot = argv[++i];
    }
    scanner s = {.storage = storage, .service = service, .copy_log = -1};
    uint64_t start = 0, budget = 10000, timeout = 60;
    if (!token || !output || !*output || tgfs_bot_channel_id(storage, &s.storage_id) ||
        tgfs_bot_channel_id(service, &s.service_id) || s.storage_id == s.service_id ||
        (locator && (decimal(locator, &start) || !start || start > 4503599627370495ULL)) ||
        (limit && (decimal(limit, &budget) || !budget || budget > TGFS_MAX_RECORDS)) ||
        (timeout_text && (decimal(timeout_text, &timeout) || !timeout || timeout > 300))) goto usage;
    s.copies = cJSON_CreateArray(); s.cleanup_pending = cJSON_CreateArray();
    int rc = 1;
    if (!s.copies || !s.cleanup_pending) {
        fprintf(stderr, "FAIL scan client: out of memory\n"); goto end;
    }
    if (tgfs_bot_open(token, (unsigned)timeout, &s.bot, &s.error) ||
        tgfs_bot_set_proxy(s.bot, proxy, &s.error)) {
        fprintf(stderr, "FAIL scan client: %s\n", s.error.message); goto end;
    }
    rc = scan(&s, output, (long long)start, (unsigned)budget);
end:

    tgfs_bot_close(s.bot); cJSON_Delete(s.copies); cJSON_Delete(s.cleanup_pending);
    return rc;
usage:
    tgfs_scan_help(stderr); return 2;
}
