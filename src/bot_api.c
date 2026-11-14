#include "tgfs/bot_api.h"
#include "internal.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define API_LIMIT (1024U * 1024U)
#define TOKEN_LIMIT 256
#define MAX_EXACT_ID 4503599627370495LL

struct tgfs_bot {
    CURL *curl;
    char token[TOKEN_LIMIT + 1];
    char base[96];
    unsigned timeout;
    char *proxy;
};
typedef struct {
    unsigned char *data;
    size_t length, limit;
    int exceeded, oom;
} response;

static void clear_error(tgfs_bot_error *e) { if (e) memset(e, 0, sizeof(*e)); }
static int bot_fail(tgfs_bot_error *e, tgfs_bot_status kind, const char *text)
{
    if (e) { e->kind = kind; snprintf(e->message, sizeof(e->message), "%s", text); }
    return -1;
}

int tgfs_bot_integer(const cJSON *object, const char *key, long long *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!out || !cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        fabs(v->valuedouble) > (double)MAX_EXACT_ID || floor(v->valuedouble) != v->valuedouble) return -1;
    *out = (long long)v->valuedouble;
    return 0;
}

int tgfs_bot_channel_id(const char *s, long long *out)
{
    uint64_t n;
    if (!s || !out || s[0] != '-' || decimal(s + 1, &n) || !n || n > MAX_EXACT_ID) return -1;
    *out = -(long long)n;
    return 0;
}

static int token_read(const char *file, char token[TOKEN_LIMIT + 1], tgfs_bot_error *e)
{
    int fd = file ? open(file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) : -1;
    if (fd < 0) return bot_fail(e, TGFS_BOT_LOCAL, "cannot open token file");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size <= 0 || st.st_size > TOKEN_LIMIT) {
        close(fd); return bot_fail(e, TGFS_BOT_LOCAL, "token file must be owner-only, regular and at most 256 bytes");
    }
    size_t done = 0, size = (size_t)st.st_size;
    while (done < size) {
        /* A token is at most 256 bytes; keep the read bound explicit for fortify. */
        ssize_t n = read(fd, token + done, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return bot_fail(e, TGFS_BOT_LOCAL, "cannot read token file"); }
        done += (size_t)n;
    }
    char extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n != 0 || memchr(token, 0, size)) return bot_fail(e, TGFS_BOT_LOCAL, "invalid token file");
    while (size && (token[size - 1] == '\n' || token[size - 1] == '\r')) token[--size] = 0;
    token[size] = 0;
    char *colon = strchr(token, ':');
    if (!colon || colon == token || !colon[1]) return bot_fail(e, TGFS_BOT_LOCAL, "invalid token syntax");
    for (const char *p = token; p < colon; p++)
        if (*p < '0' || *p > '9') return bot_fail(e, TGFS_BOT_LOCAL, "invalid token syntax");
    for (const char *p = colon + 1; *p; p++)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return bot_fail(e, TGFS_BOT_LOCAL, "invalid token syntax");
    return 0;
}

int tgfs_bot_open(const char *file, unsigned timeout, tgfs_bot **out, tgfs_bot_error *e)
{
    clear_error(e);
    if (!out) return bot_fail(e, TGFS_BOT_LOCAL, "missing client output");
    *out = NULL;
    if (timeout < 1 || timeout > 300) return bot_fail(e, TGFS_BOT_LOCAL, "timeout must be between 1 and 300 seconds");
    tgfs_bot *b = calloc(1, sizeof(*b));
    if (!b) return bot_fail(e, TGFS_BOT_LOCAL, "out of memory");
    if (token_read(file, b->token, e)) { OPENSSL_cleanse(b, sizeof(*b)); free(b); return -1; }
    snprintf(b->base, sizeof(b->base), "https://api.telegram.org");
#ifdef TGFS_TESTING
    /* Never compiled into build/tgfs. The test client only accepts loopback. */
    const char *test = getenv("TGFS_TEST_API_BASE");
    if (test) {
        const char *prefix = "http://127.0.0.1:";
        uint64_t port;
        if (strncmp(test, prefix, strlen(prefix)) || decimal(test + strlen(prefix), &port) ||
            port == 0 || port > 65535) {
            OPENSSL_cleanse(b, sizeof(*b)); free(b);
            return bot_fail(e, TGFS_BOT_LOCAL, "invalid loopback test endpoint");
        }
        snprintf(b->base, sizeof(b->base), "%s", test);
    }
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        OPENSSL_cleanse(b, sizeof(*b)); free(b); return bot_fail(e, TGFS_BOT_LOCAL, "cannot initialise HTTP library");
    }
    b->curl = curl_easy_init();
    b->timeout = timeout;
    if (!b->curl) { tgfs_bot_close(b); return bot_fail(e, TGFS_BOT_LOCAL, "cannot initialise HTTP client"); }
    *out = b;
    return 0;
}

int tgfs_bot_set_proxy(tgfs_bot *b, const char *url, tgfs_bot_error *e)
{
    clear_error(e);
    if (!b) return bot_fail(e, TGFS_BOT_LOCAL, "missing client");
    if (url && *url) {
        const char *host = NULL;
        const char *schemes[] = {"http://", "https://", "socks5://", "socks5h://"};
        for (size_t i = 0; i < sizeof(schemes) / sizeof(*schemes); i++)
            if (!strncmp(url, schemes[i], strlen(schemes[i]))) host = url + strlen(schemes[i]);
        if (!host || !*host || strlen(url) > 1024 || strpbrk(host, "@/?#\\ \t\r\n"))
            return bot_fail(e, TGFS_BOT_LOCAL, "invalid proxy URL (credentials are not accepted)");
    }
    char *copy = url ? strdup(url) : NULL;
    if (url && !copy) return bot_fail(e, TGFS_BOT_LOCAL, "out of memory");
    free(b->proxy);
    b->proxy = copy;
    return 0;
}

void tgfs_bot_close(tgfs_bot *b)
{
    if (!b) return;
    curl_easy_cleanup(b->curl);
    free(b->proxy);
    OPENSSL_cleanse(b, sizeof(*b));
    free(b);
    curl_global_cleanup();
}

static size_t receive(char *data, size_t size, size_t count, void *arg)
{
    response *r = arg;
    if (size && count > SIZE_MAX / size) { r->exceeded = 1; return 0; }
    size_t n = size * count;
    if (n > r->limit - r->length) { r->exceeded = 1; return 0; }
    unsigned char *p = realloc(r->data, r->length + n + 1);
    if (!p) { r->oom = 1; return 0; }
    r->data = p;
    memcpy(r->data + r->length, data, n);
    r->length += n;
    r->data[r->length] = 0;
    return n;
}

static int configure(tgfs_bot *b, const char *url, response *r, tgfs_bot_error *e)
{
    curl_easy_reset(b->curl);
#define OPT(key, value) do { if (curl_easy_setopt(b->curl, key, value) != CURLE_OK) \
    return bot_fail(e, TGFS_BOT_LOCAL, "cannot configure HTTP request"); } while (0)
    OPT(CURLOPT_URL, url);
    OPT(CURLOPT_WRITEFUNCTION, receive);
    OPT(CURLOPT_WRITEDATA, r);
    OPT(CURLOPT_CONNECTTIMEOUT, 10L);
    OPT(CURLOPT_TIMEOUT, (long)b->timeout);
    OPT(CURLOPT_NOSIGNAL, 1L);
    OPT(CURLOPT_FOLLOWLOCATION, 0L);
    OPT(CURLOPT_SSL_VERIFYPEER, 1L);
    OPT(CURLOPT_SSL_VERIFYHOST, 2L);
    OPT(CURLOPT_VERBOSE, 0L);
    OPT(CURLOPT_USERAGENT, "tgfs-transport/0.1");
#ifdef TGFS_TESTING
    OPT(CURLOPT_PROTOCOLS_STR, "http,https");
    if (!strncmp(b->base, "http://127.0.0.1:", 17)) OPT(CURLOPT_PROXY, "");
#else
    OPT(CURLOPT_PROTOCOLS_STR, "https");
#endif
    if (b->proxy) {
        OPT(CURLOPT_PROXY, b->proxy);
        OPT(CURLOPT_NOPROXY, "");
    }
#undef OPT
    return 0;
}

static int perform(tgfs_bot *b, response *r, int mutating, tgfs_bot_error *e, long *status)
{
    CURLcode rc = curl_easy_perform(b->curl);
    curl_easy_getinfo(b->curl, CURLINFO_RESPONSE_CODE, status);
    if (e) e->http_status = *status;
    if (rc == CURLE_OK) return 0;
    if (e) e->outcome_unknown = mutating && rc != CURLE_COULDNT_RESOLVE_HOST &&
        rc != CURLE_COULDNT_RESOLVE_PROXY && rc != CURLE_COULDNT_CONNECT;
    if (r->exceeded) return bot_fail(e, TGFS_BOT_LIMIT, "response exceeds configured size limit");
    if (r->oom) return bot_fail(e, TGFS_BOT_LOCAL, "out of memory receiving response");
    /* curl error buffers can contain URLs with the token; expose only a category. */
    return bot_fail(e, TGFS_BOT_NETWORK, rc == CURLE_OPERATION_TIMEDOUT
        ? "request timed out" : "network or TLS request failed");
}

static int api_result(response *r, long http, int mutating, const char *token, cJSON **result, tgfs_bot_error *e)
{
    if (e) e->outcome_unknown = mutating;
    tgfs_error parse_error;
    cJSON *root = json_parse_bytes(r->data, r->length, &parse_error);
    if (!root) return bot_fail(e, http >= 200 && http < 300 ? TGFS_BOT_FORMAT : TGFS_BOT_HTTP,
                               "invalid API response");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (cJSON_IsFalse(ok)) {
        long long code, retry = 0;
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
        if (tgfs_bot_integer(root, "error_code", &code) || code < 100 || code > 599) {
            cJSON_Delete(root); return bot_fail(e, TGFS_BOT_FORMAT, "invalid API error envelope");
        }
        if (e) {
            e->api_code = (int)code;
            const char *description = json_string(root, "description");
            e->message_unforwardable = http == 400 && code == 400 && description &&
                (!strcmp(description, "Bad Request: message can't be forwarded") ||
                 !strcmp(description, "Bad Request: Message can't be forwarded"));
            e->message_missing = http == 400 && code == 400 && description &&
                (!strcmp(description, "Bad Request: message to forward not found") ||
                 !strcmp(description, "Bad Request: MESSAGE_ID_INVALID") ||
                 !strcmp(description, "Bad Request: message_id_invalid"));
            /* 5xx after a mutating request does not prove that nothing happened. */
            e->outcome_unknown = mutating && (code >= 500 || http >= 500);
            if (!tgfs_bot_integer(parameters, "retry_after", &retry) && retry > 0 && retry <= UINT_MAX)
                e->retry_after = (unsigned)retry;
        }
        bot_fail(e, TGFS_BOT_API, "Telegram API rejected the request");
        const char *description = json_string(root, "description");
        const char *secret = strchr(token, ':');
        /* Bounded printable diagnostic; never expose credentials or URLs. */
        if (e && description && !strstr(description, token) &&
            (!secret || !strstr(description, secret + 1)) &&
            !strstr(description, "://")) {
            size_t n = strlen(description);
            if (n > 120) n = 120;
            snprintf(e->message, sizeof(e->message), "Telegram API: %.*s", (int)n, description);
            for (char *p = e->message; *p; p++)
                if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
        }
        cJSON_Delete(root);
        return -1;
    }
    if (http < 200 || http >= 300 || !cJSON_IsTrue(ok) || !cJSON_HasObjectItem(root, "result")) {
        cJSON_Delete(root); return bot_fail(e, TGFS_BOT_FORMAT, "unexpected HTTP status or API envelope");
    }
    *result = cJSON_DetachItemFromObjectCaseSensitive(root, "result");
    cJSON_Delete(root);
    if (e) e->outcome_unknown = 0;
    return 0;
}

int tgfs_bot_call(tgfs_bot *b, const char *method, const cJSON *params, cJSON **result, tgfs_bot_error *e)
{
    clear_error(e);
    if (result) *result = NULL;
    const char *allowed[] = {"getMe", "getChat", "getFile", "sendMessage", "forwardMessage",
                             "editMessageText", "editMessageCaption", "deleteMessage", "getChatMember", "pinChatMessage"};
    int index = -1;
    for (size_t i = 0; method && i < sizeof(allowed) / sizeof(*allowed); i++)
        if (!strcmp(method, allowed[i])) index = (int)i;
    if (!b || !result || index < 0 || !cJSON_IsObject(params))
        return bot_fail(e, TGFS_BOT_LOCAL, "invalid API method or parameters");
    char *body = cJSON_PrintUnformatted(params);
    if (!body) return bot_fail(e, TGFS_BOT_LOCAL, "cannot encode API request");
    if (strlen(body) > API_LIMIT) { free(body); return bot_fail(e, TGFS_BOT_LIMIT, "API request too large"); }
    char url[512];
    snprintf(url, sizeof(url), "%s/bot%s/%s", b->base, b->token, method);
    response r = {.limit = API_LIMIT};
    long status = 0;
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    int rc = -1;
    if (!headers) { bot_fail(e, TGFS_BOT_LOCAL, "out of memory"); goto done; }
    if (configure(b, url, &r, e)) goto done;
    if (curl_easy_setopt(b->curl, CURLOPT_HTTPHEADER, headers) ||
        curl_easy_setopt(b->curl, CURLOPT_POSTFIELDS, body) ||
        curl_easy_setopt(b->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body))) {
        bot_fail(e, TGFS_BOT_LOCAL, "cannot configure JSON request"); goto done;
    }
    int mutating = index >= 3 && strcmp(method, "getChatMember");
    if (!perform(b, &r, mutating, e, &status)) rc = api_result(&r, status, mutating, b->token, result, e);
done:
    curl_easy_reset(b->curl);
    curl_slist_free_all(headers);
    free(body); free(r.data);
    OPENSSL_cleanse(url, sizeof(url));
    return rc;
}

static int mime_text(curl_mime *mime, const char *name, const char *text)
{
    curl_mimepart *part = curl_mime_addpart(mime);
    return !part || curl_mime_name(part, name) || curl_mime_data(part, text, CURL_ZERO_TERMINATED);
}

int tgfs_bot_send_document(tgfs_bot *b, const char *chat_id, const char *filename,
                           const void *bytes, size_t length, const char *caption,
                           cJSON **result, tgfs_bot_error *e)
{
    clear_error(e);
    if (result) *result = NULL;
    long long chat;
    if (!b || !result || tgfs_bot_channel_id(chat_id, &chat) || !name_valid(filename) ||
        strchr(filename, '\r') || strchr(filename, '\n') || !bytes || !length ||
        length > TGFS_MAX_CHUNK || !caption || strlen(caption) > 1024)
        return bot_fail(e, TGFS_BOT_LOCAL, "invalid upload parameters or prototype size limit");
    char url[512];
    snprintf(url, sizeof(url), "%s/bot%s/sendDocument", b->base, b->token);
    response r = {.limit = API_LIMIT};
    long status = 0;
    int rc = -1;
    curl_mime *mime = NULL;
    if (configure(b, url, &r, e)) goto done;
    mime = curl_mime_init(b->curl);
    if (!mime || mime_text(mime, "chat_id", chat_id) || mime_text(mime, "caption", caption) ||
        mime_text(mime, "disable_notification", "true") ||
        mime_text(mime, "disable_content_type_detection", "true")) {
        bot_fail(e, TGFS_BOT_LOCAL, "cannot build multipart request"); goto done;
    }
    curl_mimepart *part = curl_mime_addpart(mime);
    if (!part || curl_mime_name(part, "document") || curl_mime_filename(part, filename) ||
        curl_mime_type(part, "application/octet-stream") ||
        curl_mime_data(part, bytes, length) || curl_easy_setopt(b->curl, CURLOPT_MIMEPOST, mime)) {
        bot_fail(e, TGFS_BOT_LOCAL, "cannot build document upload"); goto done;
    }
    if (!perform(b, &r, 1, e, &status)) rc = api_result(&r, status, 1, b->token, result, e);
done:
    curl_easy_reset(b->curl);
    curl_mime_free(mime); free(r.data);
    OPENSSL_cleanse(url, sizeof(url));
    return rc;
}

static int path_valid(const char *path)
{
    if (!path || !*path || *path == '/' || strlen(path) > 512) return 0;
    const char *segment = path;
    for (const char *p = path;; p++) {
        if (*p == '/' || !*p) {
            size_t n = (size_t)(p - segment);
            if (!n || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            segment = p + 1;
            if (!*p) break;
        } else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
    }
    return 1;
}

int tgfs_bot_download(tgfs_bot *b, const char *path, unsigned char **bytes, size_t *length, tgfs_bot_error *e)
{
    clear_error(e);
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!b || !bytes || !length || !path_valid(path))
        return bot_fail(e, TGFS_BOT_LOCAL, "invalid relative getFile path");
    char url[1024];
    snprintf(url, sizeof(url), "%s/file/bot%s/%s", b->base, b->token, path);
    response r = {.limit = TGFS_MAX_CHUNK};
    long status = 0;
    int rc = configure(b, url, &r, e);
    if (!rc) rc = perform(b, &r, 0, e, &status);
    if (!rc && status != 200) rc = bot_fail(e, TGFS_BOT_HTTP, "download returned unexpected HTTP status");
    if (!rc) { *bytes = r.data; *length = r.length; r.data = NULL; }
    curl_easy_reset(b->curl);
    free(r.data); OPENSSL_cleanse(url, sizeof(url));
    return rc;
}
