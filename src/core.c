#include "internal.h"
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint64_t id;
    const cJSON *caption;
    const char *type, *object_id, *document, *filename;
    cJSON *body;
} message;

struct tgfs {
    int dirfd;
    tgfs_chunk_loader loader;
    void *loader_context;
    cJSON *snapshot;
    const char *root_id;
    const cJSON *root_entries;
    cJSON *directory_views;
    message *messages;
    size_t count;
};

static int same(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static int nullable_id(const cJSON *o, const char *field)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, field);
    uint64_t id;
    return cJSON_IsNull(v) || (cJSON_IsString(v) && !decimal(v->valuestring, &id) && id > 0);
}

static int envelope(const cJSON *o, const char *type, tgfs_error *e)
{
    size_t version;
    if (!cJSON_IsObject(o) || !same(json_string(o, "magic"), "TGFS") ||
        json_count(o, "format_version", &version) || version != 1 ||
        !same(json_string(o, "type"), type) || !nullable_id(o, "next_message"))
        return fail(e, "invalid %s envelope or unsupported format_version", type);
    return 0;
}

static int entries_valid(const cJSON *entries, tgfs_error *e)
{
    if (!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) > TGFS_MAX_RECORDS)
        return fail(e, "invalid directory entries");
    const cJSON *entry, *other;
    cJSON_ArrayForEach(entry, entries) {
        const char *name = json_string(entry, "name");
        if (!cJSON_IsObject(entry) || !name_valid(name) ||
            !uuid_valid(json_string(entry, "object_id")))
            return fail(e, "invalid directory entry name or object_id");
        for (other = entry->next; other; other = other->next)
            if (same(name, json_string(other, "name")))
                return fail(e, "duplicate directory entry: %s", name);
    }
    return 0;
}

static int commit_valid(message *m, tgfs_error *e)
{
    const cJSON *c = m->caption;
    uint64_t size;
    size_t count;
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(m->body, "parts");
    if (!uuid_valid(json_string(c, "operation_id")) ||
        !uuid_valid(json_string(c, "version_id")) ||
        !hash_valid(json_string(c, "file_sha256")) ||
        json_decimal(c, "file_size", &size) || json_count(c, "part_count", &count) ||
        !cJSON_IsArray(parts) || (size_t)cJSON_GetArraySize(parts) != count ||
        ((size == 0) != (count == 0)))
        return fail(e, "invalid FILE_COMMIT metadata at message %" PRIu64, m->id);
    uint64_t end = 0;
    const cJSON *p, *other;
    cJSON_ArrayForEach(p, parts) {
        uint64_t id, offset, length;
        if (json_decimal(p, "message_id", &id) || id == 0 || id >= m->id ||
            json_decimal(p, "offset", &offset) || offset != end ||
            json_decimal(p, "part_size", &length) || length == 0 ||
            length > TGFS_MAX_CHUNK || length > size - end ||
            !hash_valid(json_string(p, "part_sha256")))
            return fail(e, "invalid FILE_COMMIT part range or reference");
        for (other = p->next; other; other = other->next)
            if (same(json_string(p, "message_id"), json_string(other, "message_id")))
                return fail(e, "duplicate CHUNK reference");
        end += length;
    }
    if (end != size) return fail(e, "FILE_COMMIT parts do not cover file_size");
    return 0;
}

static int by_id(const void *a, const void *b)
{
    uint64_t x = ((const message *)a)->id, y = ((const message *)b)->id;
    return (x > y) - (x < y);
}

static message *message_find(tgfs *fs, uint64_t id)
{
    message key = {.id = id};
    return bsearch(&key, fs->messages, fs->count, sizeof(message), by_id);
}

static int directory_known(tgfs *fs, const char *id)
{
    if (same(id, fs->root_id)) return 1;
    for (size_t i = 0; i < fs->count; i++)
        if (same(fs->messages[i].type, "DIRECTORY") && same(fs->messages[i].object_id, id)) return 1;
    return 0;
}

int tgfs_open_snapshot(const char *directory, tgfs **out, tgfs_error *e)
{
    return tgfs_open_snapshot_loader(directory,NULL,NULL,out,e);
}

int tgfs_open_snapshot_loader(const char *directory, tgfs_chunk_loader loader,
                             void *context, tgfs **out, tgfs_error *e)
{
    if (!out) return fail(e, "missing output pointer");
    *out = NULL;
    if (!directory) return fail(e, "missing snapshot directory");
    tgfs *fs = calloc(1, sizeof(*fs));
    if (!fs) return fail(e, "out of memory");
    fs->loader=loader; fs->loader_context=context;
    fs->dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fs->dirfd < 0) { tgfs_close(fs); return fail(e, "cannot open snapshot directory"); }
    fs->snapshot = json_load_at(fs->dirfd, "snapshot.json", e);
    if (!fs->snapshot) goto invalid;
    const cJSON *sb = cJSON_GetObjectItemCaseSensitive(fs->snapshot, "superblock");
    uint64_t start;
    if (envelope(sb, "SUPERBLOCK", e)) goto invalid;
    fs->root_id = json_string(sb, "root_object_id");
    if (!uuid_valid(fs->root_id) || json_decimal(sb, "start_id", &start) || !start) {
        fail(e, "invalid SUPERBLOCK root or start_id"); goto invalid;
    }
    fs->root_entries = cJSON_GetObjectItemCaseSensitive(fs->snapshot, "root_entries");
    if (fs->root_entries && entries_valid(fs->root_entries, e)) goto invalid;
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(fs->snapshot, "messages");
    if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) > TGFS_MAX_RECORDS) {
        fail(e, "invalid snapshot messages"); goto invalid;
    }
    fs->count = (size_t)cJSON_GetArraySize(messages);
    fs->messages = calloc(fs->count ? fs->count : 1, sizeof(message));
    if (!fs->messages) { fail(e, "out of memory"); goto invalid; }
    const cJSON *r;
    size_t index = 0;
    cJSON_ArrayForEach(r, messages) {
        message *m = &fs->messages[index++];
        m->caption = cJSON_GetObjectItemCaseSensitive(r, "caption");
        m->type = json_string(m->caption, "type");
        m->object_id = json_string(m->caption, "object_id");
        m->document = json_string(r, "document");
        m->filename = json_string(r, "filename");
        if (json_decimal(r, "message_id", &m->id) || m->id <= start ||
            !uuid_valid(m->object_id) || !name_valid(m->document) ||
            !(same(m->type, "CHUNK") || same(m->type, "DIRECTORY") || same(m->type, "FILE_COMMIT"))) {
            fail(e, "invalid snapshot record or unsupported message type"); goto invalid;
        }
        if (envelope(m->caption, m->type, e)) goto invalid;
        if (same(m->object_id, fs->root_id) && !same(m->type, "DIRECTORY")) {
            fail(e, "root object is supplied by snapshot root_entries, not message records"); goto invalid;
        }
        if (!same(m->type, "CHUNK")) {
            m->body = json_load_at(fs->dirfd, m->document, e);
            if (!m->body || !cJSON_IsObject(m->body)) {
                if (m->body) fail(e, "document must be a JSON object");
                goto invalid;
            }
        }
        if (same(m->type, "DIRECTORY")) {
            const cJSON *parent=cJSON_GetObjectItemCaseSensitive(m->caption,"parent_id");
            int root=same(m->object_id,fs->root_id);
            if ((root ? !cJSON_IsNull(parent) : !uuid_valid(json_string(m->caption,"parent_id"))) ||
                (cJSON_HasObjectItem(m->body,"operations") ? directory_operations_valid(m->body,e) :
                 entries_valid(cJSON_GetObjectItemCaseSensitive(m->body,"entries"),e))) {
                fail(e,"invalid DIRECTORY metadata or entries/operations");goto invalid;
            }
        }
        if (same(m->type, "FILE_COMMIT") && commit_valid(m, e)) goto invalid;
    }
    qsort(fs->messages, fs->count, sizeof(message), by_id);
    for (size_t i = 0; i < fs->count; i++) {
        message *a = &fs->messages[i];
        if (i && a->id == fs->messages[i - 1].id) {
            fail(e, "duplicate message_id"); goto invalid;
        }
        if (same(a->type, "DIRECTORY") && !same(a->object_id, fs->root_id) && !directory_known(fs, json_string(a->caption, "parent_id"))) {
            fail(e, "DIRECTORY parent_id is not a directory"); goto invalid;
        }
        for (size_t j = 0; j < i; j++) {
            message *b = &fs->messages[j];
            if (same(a->object_id, b->object_id) &&
                ((same(a->type, "DIRECTORY") && !same(b->type, "DIRECTORY")) ||
                 (same(b->type, "DIRECTORY") && !same(a->type, "DIRECTORY")))) {
                fail(e, "object_id has inconsistent object types"); goto invalid;
            }
            if (same(a->type,"DIRECTORY") && same(b->type,"DIRECTORY") && same(a->object_id,b->object_id) &&
                ((!same(a->object_id,fs->root_id) && !same(json_string(a->caption,"parent_id"),json_string(b->caption,"parent_id"))) ||
                 cJSON_HasObjectItem(a->body,"operations") != cJSON_HasObjectItem(b->body,"operations"))) {
                fail(e,"DIRECTORY parent or document mode changed");goto invalid;
            }
            if (same(a->type, "FILE_COMMIT") && same(b->type, "FILE_COMMIT") &&
                same(json_string(a->caption, "operation_id"), json_string(b->caption, "operation_id")) &&
                (!json_semantic_equal(a->caption, b->caption) || !cJSON_Compare(a->body, b->body, 1))) {
                fail(e, "operation_id reused with different commit contents"); goto invalid;
            }
        }
    }
    tgfs_directory_batch *batches=calloc(fs->count ? fs->count : 1,sizeof(*batches));
    if(!batches) {fail(e,"out of memory");goto invalid;}
    size_t batch_count=0;
    int root_record=0;
    for(size_t i=0;i<fs->count;i++) if(same(fs->messages[i].type,"DIRECTORY")) {
        message *m=&fs->messages[i];
        batches[batch_count++]=(tgfs_directory_batch){m->id,m->object_id,m->body};
        if(same(m->object_id,fs->root_id)) root_record=1;
    }
    if(root_record && fs->root_entries && cJSON_GetArraySize(fs->root_entries)) {
        free(batches);fail(e,"root_entries cannot be mixed with root DIRECTORY");goto invalid;
    }
    fs->directory_views=directory_merge(batches,batch_count,e);
    free(batches);
    if(!fs->directory_views) goto invalid;
    if(!root_record && !fs->root_entries) {
        cJSON *empty=cJSON_CreateArray();
        if(!empty || !cJSON_AddItemToObject(fs->directory_views,fs->root_id,empty)) {
            cJSON_Delete(empty);fail(e,"out of memory");goto invalid;
        }
    }
    *out = fs;
    return 0;
invalid:
    tgfs_close(fs);
    return -1;
}

void tgfs_close(tgfs *fs)
{
    if (!fs) return;
    if (fs->messages)
        for (size_t i = 0; i < fs->count; i++) cJSON_Delete(fs->messages[i].body);
    free(fs->messages);
    cJSON_Delete(fs->directory_views);
    cJSON_Delete(fs->snapshot);
    if (fs->dirfd >= 0) close(fs->dirfd);
    free(fs);
}

static const cJSON *directory_entries(tgfs *fs, const char *id, tgfs_error *e)
{
    const cJSON *merged=cJSON_GetObjectItemCaseSensitive(fs->directory_views,id);
    if(merged) return merged;
    if (same(id, fs->root_id) && fs->root_entries && cJSON_GetArraySize(fs->root_entries)) return fs->root_entries;
    const message *found = NULL;
    for (size_t i = 0; i < fs->count; i++) {
        message *m = &fs->messages[i];
        if (!same(m->object_id, id) || !same(m->type, "DIRECTORY")) continue;
        if (found) { fail(e, "ambiguous DIRECTORY: publication selection is not defined"); return NULL; }
        found = m;
    }
    if (!found) {
        if(same(id,fs->root_id)) return fs->root_entries;
        fail(e, "path component is not an available directory"); return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(found->body, "entries");
}

static const char *resolve(tgfs *fs, const char *path, tgfs_error *e)
{
    if (!path || path[0] != '/' || strlen(path) > 4096) {
        fail(e, "expected an absolute TGFS path of at most 4096 bytes"); return NULL;
    }
    char *copy = strdup(path);
    if (!copy) { fail(e, "out of memory"); return NULL; }
    const char *id = fs->root_id;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!name_valid(part)) { fail(e, "unsupported path component"); id = NULL; break; }
        const cJSON *entries = directory_entries(fs, id, e);
        if (!entries) { id = NULL; break; }
        const cJSON *entry;
        const char *child = NULL;
        cJSON_ArrayForEach(entry, entries)
            if (same(part, json_string(entry, "name"))) { child = json_string(entry, "object_id"); break; }
        if (!child) { fail(e, "path does not exist: %s", part); id = NULL; break; }
        id = child;
    }
    if (id && path[strlen(path) - 1] == '/' && !directory_entries(fs, id, e)) id = NULL;
    free(copy);
    return id;
}

int tgfs_list(tgfs *fs, const char *path, tgfs_entry_fn emit, void *arg, tgfs_error *e)
{
    const char *id = resolve(fs, path, e);
    if (!id) return -1;
    const cJSON *entries = directory_entries(fs, id, e);
    if (!entries) return -1;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, entries)
        if (emit(json_string(entry, "name"), json_string(entry, "object_id"), arg))
            return fail(e, "cannot write directory listing");
    return 0;
}

static message *selected_commit(tgfs *fs, const char *id, tgfs_error *e)
{
    message *selected = NULL;
    for (size_t i = 0; i < fs->count; i++) {
        message *m = &fs->messages[i];
        if (!same(m->type, "FILE_COMMIT") || !same(m->object_id, id)) continue;
        int repeat = 0;
        for (size_t j = 0; j < i; j++) {
            message *previous = &fs->messages[j];
            if (same(previous->type, "FILE_COMMIT") &&
                same(json_string(previous->caption, "operation_id"), json_string(m->caption, "operation_id"))) {
                repeat = 1; break;
            }
        }
        if (!repeat) selected = m;
    }
    if (!selected) fail(e, "file has no published FILE_COMMIT");
    return selected;
}

static int digest(const void *data, size_t length, char hex[65])
{
    unsigned char hash[32]; unsigned n;
    if (!EVP_Digest(data, length, hash, &n, EVP_sha256(), NULL) || n != 32) return -1;
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return 0;
}

static int chunk_matches(tgfs *fs, message *chunk, message *commit, const cJSON *part,
                         size_t index, tgfs_error *e)
{
    const cJSON *c = chunk->caption, *f = commit->caption;
    uint64_t size, offset, total, backlink, suffix;
    size_t count, expected;
    if (!same(chunk->type, "CHUNK") || !same(chunk->object_id, commit->object_id) ||
        !same(json_string(c, "version_id"), json_string(f, "version_id")) ||
        !same(json_string(c, "file_sha256"), json_string(f, "file_sha256")) ||
        !same(json_string(c, "file_size"), json_string(f, "file_size")) ||
        !same(json_string(c, "offset"), json_string(part, "offset")) ||
        !same(json_string(c, "part_size"), json_string(part, "part_size")) ||
        !same(json_string(c, "part_sha256"), json_string(part, "part_sha256")) ||
        json_decimal(c, "offset", &offset) || json_decimal(c, "part_size", &size) ||
        json_decimal(c, "file_size", &total) || json_count(c, "part_count", &count) ||
        json_count(f, "part_count", &expected) || count != expected ||
        !uuid_valid(json_string(c, "parent_id")) ||
        !directory_known(fs, json_string(c, "parent_id")) ||
        cJSON_HasObjectItem(c, "part_index") || !nullable_id(c, "file_commit_id"))
        return fail(e, "CHUNK metadata disagrees with FILE_COMMIT");
    const cJSON *back = cJSON_GetObjectItemCaseSensitive(c, "file_commit_id");
    if (!cJSON_IsNull(back) && (decimal(back->valuestring, &backlink) || backlink != commit->id))
        return fail(e, "CHUNK backlink disagrees with canonical FILE_COMMIT");
    if (!chunk->filename || !name_valid(chunk->filename)) return fail(e, "missing CHUNK filename");
    const char *dot = strrchr(chunk->filename, '.');
    if (!dot || dot == chunk->filename || decimal(dot + 1, &suffix) || suffix != index || suffix >= count)
        return fail(e, "invalid CHUNK filename suffix");
    return 0;
}

int tgfs_cat(tgfs *fs, const char *path, FILE *output, tgfs_error *e)
{
    const char *id = resolve(fs, path, e);
    if (!id) return -1;
    if (directory_known(fs, id)) return fail(e, "cannot read a directory as a file");
    message *commit = selected_commit(fs, id, e);
    if (!commit) return -1;
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(commit->body, "parts");
    /* Check every reference before emitting a prefix. Byte checks remain streaming. */
    const cJSON *p; size_t index = 0;
    cJSON_ArrayForEach(p, parts) {
        uint64_t mid;
        if (json_decimal(p, "message_id", &mid)) return fail(e, "invalid part ID");
        message *chunk = message_find(fs, mid);
        if (!chunk) return fail(e, "missing CHUNK message %" PRIu64, mid);
        if (chunk_matches(fs, chunk, commit, p, index++, e)) return -1;
    }
    EVP_MD_CTX *full = EVP_MD_CTX_new();
    if (!full || !EVP_DigestInit_ex(full, EVP_sha256(), NULL)) {
        EVP_MD_CTX_free(full); return fail(e, "cannot initialise SHA-256");
    }
    int result = -1;
    cJSON_ArrayForEach(p, parts) {
        uint64_t mid, expected;
        if (json_decimal(p, "message_id", &mid) || json_decimal(p, "part_size", &expected)) {
            fail(e, "invalid part metadata"); goto done;
        }
        message *chunk = message_find(fs, mid);
        size_t length;
        unsigned char *bytes = fs->loader ? fs->loader(mid,&length,fs->loader_context,e) :
            bytes_load_at(fs->dirfd, chunk->document, TGFS_MAX_CHUNK, &length, e);
        if (!bytes) goto done;
        char hash[65];
        if (length != expected || digest(bytes, length, hash) ||
            !same(hash, json_string(p, "part_sha256"))) {
            free(bytes); fail(e, "CHUNK %" PRIu64 " size or SHA-256 mismatch", mid); goto done;
        }
        if (!EVP_DigestUpdate(full, bytes, length)) {
            free(bytes); fail(e, "SHA-256 update failed"); goto done;
        }
        if (fwrite(bytes, 1, length, output) != length) {
            free(bytes); fail(e, "cannot write file output"); goto done;
        }
        free(bytes);
    }
    unsigned char bytes[32]; unsigned len;
    char hash[65];
    if (!EVP_DigestFinal_ex(full, bytes, &len) || len != 32) { fail(e, "SHA-256 final failed"); goto done; }
    for (size_t i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", bytes[i]);
    if (!same(hash, json_string(commit->caption, "file_sha256"))) {
        fail(e, "whole-file SHA-256 mismatch"); goto done;
    }
    if (fflush(output)) { fail(e, "cannot flush file output"); goto done; }
    result = 0;
done:
    EVP_MD_CTX_free(full);
    return result;
}
