#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fail(tgfs_error *e, const char *format, ...)
{
    if (e) {
        e->code=EIO;
        va_list ap;
        va_start(ap, format);
        vsnprintf(e->message, sizeof(e->message), format, ap);
        va_end(ap);
    }
    return -1;
}

int fail_code(tgfs_error *e,int code,const char *format,...)
{
    if(e) {
        e->code=code;
        va_list ap;va_start(ap,format);vsnprintf(e->message,sizeof(e->message),format,ap);va_end(ap);
    }
    return -1;
}

unsigned char *bytes_load_at(int dirfd, const char *file, size_t limit,
                             size_t *length, tgfs_error *e)
{
    if (!name_valid(file)) { fail(e, "invalid snapshot document name"); return NULL; }
    int fd = openat(dirfd, file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { fail(e, "cannot open document %s: %s", file, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > limit) {
        close(fd); fail(e, "document %s is not a regular file within size limit", file); return NULL;
    }
    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size + 1);
    if (!data) { close(fd); fail(e, "out of memory"); return NULL; }
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { fail(e, "cannot read document %s", file); free(data); close(fd); return NULL; }
        done += (size_t)n;
    }
    unsigned char extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n != 0) { free(data); fail(e, "document %s changed during read", file); return NULL; }
    data[size] = 0;
    *length = size;
    return data;
}

/* cJSON does not validate UTF-8 and stores strings as NUL-terminated buffers. */
static int utf8_valid(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], cp;
        size_t more;
        if (c == 0) return 0;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { more = 1; cp = c & 31; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; cp = c & 15; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; cp = c & 7; }
        else return 0;
        if (n - i < more) return 0;
        for (size_t k = 0; k < more; k++) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (c & 63);
        }
        if ((more == 1 && cp < 0x80) || (more == 2 && cp < 0x800) ||
            (more == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}

static int preflight(const unsigned char *s, size_t n)
{
    if (!utf8_valid(s, n) || (n >= 3 && !memcmp(s, "\xef\xbb\xbf", 3))) return 0;
    int quoted = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"') { quoted = !quoted; continue; }
        if (quoted) {
            if (s[i] < 0x20) return 0;
            if (s[i] == '\\') {
                if (++i >= n) return 0;
                if (s[i] == 'u' && n - i >= 5 && !memcmp(s + i + 1, "0000", 4)) return 0;
            }
        } else if (s[i] == '-' || (s[i] >= '0' && s[i] <= '9')) {
            if (s[i] == '-' && ++i >= n) return 0;
            if (s[i] == '0') i++;
            else {
                if (s[i] < '1' || s[i] > '9') return 0;
                while (i < n && s[i] >= '0' && s[i] <= '9') i++;
            }
            if (i < n && s[i] == '.') {
                i++; size_t begin = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') i++;
                if (i == begin) return 0;
            }
            if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                i++;
                if (i < n && (s[i] == '+' || s[i] == '-')) i++;
                size_t begin = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') i++;
                if (i == begin) return 0;
            }
            if (i < n && !strchr(" \r\n\t,}]", s[i])) return 0;
            if (i) i--;
        } else if (s[i] < 0x20 && !strchr("\r\n\t", s[i])) return 0;
    }
    return !quoted;
}

static int unique_keys(const cJSON *node, unsigned depth)
{
    if (depth > 64) return 0;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, node) {
        if (cJSON_IsObject(node))
            for (b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string)) return 0;
        if (!unique_keys(a, depth + 1)) return 0;
    }
    return 1;
}

cJSON *json_parse_bytes(const unsigned char *raw, size_t len, tgfs_error *e)
{
    cJSON *root = NULL;
    if (raw && len <= TGFS_MAX_JSON && preflight(raw, len)) {
        char *copy = malloc(len + 1);
        if (!copy) { fail(e, "out of memory"); return NULL; }
        memcpy(copy, raw, len);
        copy[len] = 0;
        root = cJSON_ParseWithLengthOpts(copy, len + 1, NULL, 1);
        free(copy);
    }
    if (!root || !unique_keys(root, 0)) {
        cJSON_Delete(root); fail(e, "invalid JSON or duplicate keys"); return NULL;
    }
    return root;
}

cJSON *json_load_at(int dirfd, const char *file, tgfs_error *e)
{
    size_t len;
    unsigned char *raw = bytes_load_at(dirfd, file, TGFS_MAX_JSON, &len, e);
    if (!raw) return NULL;
    cJSON *root = json_parse_bytes(raw, len, e);
    free(raw);
    return root;
}

const char *json_string(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

int decimal(const char *s, uint64_t *out)
{
    if (!s || !*s || (s[0] == '0' && s[1])) return -1;
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > ((uint64_t)INT64_MAX - (unsigned)(*s - '0')) / 10)
            return -1;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return 0;
}

int json_decimal(const cJSON *o, const char *key, uint64_t *out)
{
    return decimal(json_string(o, key), out);
}

int json_count(const cJSON *o, const char *key, size_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > TGFS_MAX_RECORDS || floor(v->valuedouble) != v->valuedouble) return -1;
    *out = (size_t)v->valuedouble;
    return 0;
}

static int hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
int uuid_valid(const char *s)
{
    if (!s || strlen(s) != 36) return 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return 0; }
        else if (!hex_lower(s[i])) return 0;
    }
    return s[14] == '4' && strchr("89ab", s[19]) != NULL;
}
int hash_valid(const char *s)
{
    if (!s || strlen(s) != 64) return 0;
    for (size_t i = 0; i < 64; i++) if (!hex_lower(s[i])) return 0;
    return 1;
}
int name_valid(const char *s)
{
    return s && *s && strlen(s) <= 255 && !strchr(s, '/') &&
           strcmp(s, ".") && strcmp(s, "..");
}

/* Ignore only the top-level mutable navigation field of a commit. */
int json_semantic_equal(const cJSON *a, const cJSON *b)
{
    cJSON *x = cJSON_Duplicate(a, 1), *y = cJSON_Duplicate(b, 1);
    if (!x || !y) { cJSON_Delete(x); cJSON_Delete(y); return 0; }
    cJSON_DeleteItemFromObjectCaseSensitive(x, "next_message");
    cJSON_DeleteItemFromObjectCaseSensitive(y, "next_message");
    int equal = cJSON_Compare(x, y, 1);
    cJSON_Delete(x); cJSON_Delete(y);
    return equal;
}
