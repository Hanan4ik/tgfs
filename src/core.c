#include "internal.h"
#include <fcntl.h>
#include <errno.h>
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
    unsigned char *last_chunk;
    uint64_t last_chunk_id;
    size_t last_chunk_size;
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
    free(fs->last_chunk);
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
        fail_code(e, ENOTDIR, "path component is not an available directory"); return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(found->body, "entries");
}

static const char *resolve(tgfs *fs, const char *path, tgfs_error *e)
{
    if (!path || path[0] != '/' || strlen(path) > 4096) {
        fail_code(e, path && strlen(path)>4096 ? ENAMETOOLONG : EINVAL, "expected an absolute TGFS path of at most 4096 bytes"); return NULL;
    }
    char *copy = strdup(path);
    if (!copy) { fail(e, "out of memory"); return NULL; }
    const char *id = fs->root_id;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!name_valid(part)) { fail_code(e, EINVAL, "unsupported path component"); id = NULL; break; }
        const cJSON *entries = directory_entries(fs, id, e);
        if (!entries) { id = NULL; break; }
        const cJSON *entry;
        const char *child = NULL;
        cJSON_ArrayForEach(entry, entries)
            if (same(part, json_string(entry, "name"))) { child = json_string(entry, "object_id"); break; }
        if (!child) { fail_code(e, ENOENT, "path does not exist: %s", part); id = NULL; break; }
        id = child;
    }
    if (id && path[strlen(path) - 1] == '/' && !directory_entries(fs, id, e)) id = NULL;
    free(copy);
    return id;
}

/* Resolve a captured directory once; publication keeps these UUIDs even if Telegram IDs change. */
int directory_target(tgfs *fs,const char *path,const char *name,char object[37],char parent[37],tgfs_error *e)
{
    const char *id=resolve(fs,path,e);if(!id) return -1;
    const cJSON *entries=directory_entries(fs,id,e),*entry;
    if(!entries) return -1;
    cJSON_ArrayForEach(entry,entries)
        if(same(name,json_string(entry,"name"))) return fail_code(e,EEXIST,"name already exists in captured directory");
    memcpy(object,id,37);parent[0]=0;
    if(!same(id,fs->root_id)) {
        for(size_t i=0;i<fs->count;i++) if(same(fs->messages[i].type,"DIRECTORY") && same(fs->messages[i].object_id,id)) {
            const char *pid=json_string(fs->messages[i].caption,"parent_id");
            if(!uuid_valid(pid)) return fail(e,"invalid directory parent");
            memcpy(parent,pid,37);return 0;
        }
        return fail(e,"missing directory metadata");
    }
    return 0;
}

int unlink_target(tgfs *fs,const char *path,const char *name,char object[37],char parent[37],
                  char grandparent[37],cJSON **observed,tgfs_error *e)
{
    *observed=NULL;
    const char *id=resolve(fs,path,e);if(!id) return -1;
    const cJSON *entries=directory_entries(fs,id,e),*entry;
    if(!entries) return -1;
    cJSON_ArrayForEach(entry,entries) if(same(name,json_string(entry,"name"))) break;
    if(!entry) return fail_code(e,ENOENT,"unlink target does not exist in capture");
    const char *child=json_string(entry,"object_id");tgfs_attr attr;
    if(tgfs_object_attr(fs,child,&attr,e)) return -1;
    if(attr.directory) return fail_code(e,EISDIR,"unlink target is a directory; rmdir is not implemented");
    memcpy(object,child,37);memcpy(parent,id,37);grandparent[0]=0;
    tgfs_directory_batch *batches=calloc(fs->count?fs->count:1,sizeof(*batches));
    if(!batches) return fail(e,"out of memory");
    size_t count=0;
    for(size_t i=0;i<fs->count;i++) {
        message *m=&fs->messages[i];if(!same(m->type,"DIRECTORY")) continue;
        if(same(m->object_id,id)) {
            if(!cJSON_HasObjectItem(m->body,"operations")) {free(batches);return fail(e,"unlink requires DIRECTORY operations; legacy entries are unsupported");}
            if(!same(id,fs->root_id)) memcpy(grandparent,json_string(m->caption,"parent_id"),37);
        }
        batches[count++]=(tgfs_directory_batch){m->id,m->object_id,m->body};
    }
    *observed=directory_observed_adds(batches,count,id,name,child,e);free(batches);
    return *observed?0:-1;
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

int replacement_target(tgfs *fs,const char *path,const char *name,char object[37],char parent[37],
                       char version[37],uint64_t *commit_id,tgfs_error *e)
{
    const char *id=resolve(fs,path,e);if(!id) return -1;
    const cJSON *entries=directory_entries(fs,id,e),*entry;
    if(!entries) return -1;
    cJSON_ArrayForEach(entry,entries) if(same(name,json_string(entry,"name"))) {
        const char *child=json_string(entry,"object_id");
        if(directory_known(fs,child)) return fail_code(e,EISDIR,"replace target is a directory");
        message *commit=selected_commit(fs,child,e);if(!commit) return -1;
        if(!same(json_string(commit->caption,"parent_id"),id))
            return fail(e,"replace target parent disagrees with selected FILE_COMMIT");
        memcpy(object,child,37);memcpy(parent,id,37);
        memcpy(version,json_string(commit->caption,"version_id"),37);*commit_id=commit->id;return 0;
    }
    return fail_code(e,ENOENT,"replace target does not exist in capture");
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

typedef struct { message *chunk;const cJSON *metadata;uint64_t offset,size; } file_part;
struct tgfs_file {
    tgfs *fs;
    message *commit;
    file_part *parts;
    size_t count;
    tgfs_attr attr;
    EVP_MD_CTX *sequential;
    uint64_t verified_until;
    int hash_failed;
};

int tgfs_object_attr(tgfs *fs,const char *id,tgfs_attr *attr,tgfs_error *e)
{
    if(!fs || !id || !attr) return fail_code(e,EINVAL,"missing attribute arguments");
    memset(attr,0,sizeof(*attr));
    attr->directory=directory_known(fs,id);
    if(same(id,fs->root_id)) attr->inode=1;
    else for(size_t i=0;i<fs->count;i++) if(same(fs->messages[i].object_id,id)) {attr->inode=i+2;break;}
    if(!attr->inode) return fail(e,"entry refers to an unavailable object");
    attr->links=1;
    if(attr->directory) {
        const cJSON *entries=directory_entries(fs,id,e),*entry;
        if(!entries) return -1;
        attr->links=2;
        cJSON_ArrayForEach(entry,entries) if(directory_known(fs,json_string(entry,"object_id"))) attr->links++;
        return 0;
    }
    message *commit=selected_commit(fs,id,e);
    if(!commit) return -1;
    if(json_decimal(commit->caption,"file_size",&attr->size) || attr->size>INT64_MAX)
        return fail_code(e,EOVERFLOW,"file size cannot be represented by POSIX offsets");
    return 0;
}
int tgfs_getattr(tgfs *fs,const char *path,tgfs_attr *attr,tgfs_error *e)
{
    const char *id=resolve(fs,path,e);
    return id ? tgfs_object_attr(fs,id,attr,e) : -1;
}
int tgfs_file_open(tgfs *fs,const char *path,tgfs_file **out,tgfs_error *e)
{
    if(!out) return fail_code(e,EINVAL,"missing file output");
    *out=NULL;
    const char *id=resolve(fs,path,e);if(!id) return -1;
    if(directory_known(fs,id)) return fail_code(e,EISDIR,"cannot open a directory as a file");
    tgfs_file *file=calloc(1,sizeof(*file));
    if(!file) return fail_code(e,ENOMEM,"out of memory opening file");
    file->fs=fs;file->commit=selected_commit(fs,id,e);
    if(!file->commit || tgfs_object_attr(fs,id,&file->attr,e)) goto invalid;
    const cJSON *parts=cJSON_GetObjectItemCaseSensitive(file->commit->body,"parts");
    file->count=(size_t)cJSON_GetArraySize(parts);
    file->parts=calloc(file->count ? file->count : 1,sizeof(*file->parts));
    if(!file->parts) {fail_code(e,ENOMEM,"out of memory opening parts");goto invalid;}
    const cJSON *p;size_t i=0;
    cJSON_ArrayForEach(p,parts) {
        uint64_t mid;
        if(json_decimal(p,"message_id",&mid)) {fail(e,"invalid part ID");goto invalid;}
        message *chunk=message_find(fs,mid);
        if(!chunk) {fail(e,"missing CHUNK message %" PRIu64,mid);goto invalid;}
        if(chunk_matches(fs,chunk,file->commit,p,i,e)) goto invalid;
        file->parts[i].chunk=chunk;file->parts[i].metadata=p;
        if(json_decimal(p,"offset",&file->parts[i].offset) || json_decimal(p,"part_size",&file->parts[i].size)) {
            fail(e,"invalid file range");goto invalid;
        }
        i++;
    }
    if(!file->attr.size) {
        char hash[65];
        if(digest("",0,hash) || !same(hash,json_string(file->commit->caption,"file_sha256"))) {
            fail(e,"empty-file SHA-256 mismatch");goto invalid;
        }
    }
    *out=file;return 0;
invalid:
    tgfs_file_close(file);return -1;
}
void tgfs_file_close(tgfs_file *file)
{
    if(file) {free(file->parts);EVP_MD_CTX_free(file->sequential);free(file);}
}
void tgfs_file_attr(const tgfs_file *file,tgfs_attr *attr) {*attr=file->attr;}
cJSON *tgfs_file_references(const tgfs_file *file)
{
    cJSON *refs=cJSON_CreateObject();
    char commit_id[32];
    snprintf(commit_id,sizeof(commit_id),"%" PRIu64,file->commit->id);
    if(!refs) return NULL;
    cJSON *chunks=cJSON_AddArrayToObject(refs,"chunk_ids");
    if(!chunks || !cJSON_AddStringToObject(refs,"commit_id",commit_id)) goto invalid;
    for(size_t i=0;i<file->count;i++) {
        const char *id=json_string(file->parts[i].metadata,"message_id");
        cJSON *value=cJSON_CreateString(id);
        if(!value || !cJSON_AddItemToArray(chunks,value)) {cJSON_Delete(value);goto invalid;}
    }
    return refs;
invalid:
    cJSON_Delete(refs);return NULL;
}

static int part_bytes(tgfs_file *file,file_part *part,tgfs_error *e)
{
    tgfs *fs=file->fs;
    if(fs->last_chunk && fs->last_chunk_id==part->chunk->id) return 0;
    size_t length=0;
    unsigned char *bytes=fs->loader ? fs->loader(part->chunk->id,&length,fs->loader_context,e) :
        bytes_load_at(fs->dirfd,part->chunk->document,TGFS_MAX_CHUNK,&length,e);
    if(!bytes) return -1;
    char hash[65];
    if(length!=part->size || digest(bytes,length,hash) || !same(hash,json_string(part->metadata,"part_sha256"))) {
        free(bytes);return fail(e,"CHUNK size or SHA-256 mismatch");
    }
    free(fs->last_chunk);fs->last_chunk=bytes;fs->last_chunk_size=length;fs->last_chunk_id=part->chunk->id;return 0;
}
int tgfs_file_read(tgfs_file *file,uint64_t offset,void *buffer,size_t capacity,size_t *length,tgfs_error *e)
{
    if(!file || !length || (!buffer && capacity)) return fail_code(e,EINVAL,"invalid read arguments");
    *length=0;
    if(file->hash_failed) return fail(e,"file SHA-256 previously failed");
    if(offset>=file->attr.size || !capacity) return 0;
    size_t wanted=capacity;
    if((uint64_t)wanted>file->attr.size-offset) wanted=(size_t)(file->attr.size-offset);
    size_t lo=0,hi=file->count;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if(file->parts[mid].offset+file->parts[mid].size<=offset) lo=mid+1;else hi=mid;
    }
    size_t copied=0;
    for(size_t i=lo;i<file->count && copied<wanted;i++) {
        file_part *p=&file->parts[i];
        if(part_bytes(file,p,e)) return -1;
        uint64_t current=offset+copied;
        size_t within=(size_t)(current-p->offset),take=(size_t)p->size-within;
        if(take>wanted-copied) take=wanted-copied;
        memcpy((unsigned char *)buffer+copied,file->fs->last_chunk+within,take);copied+=take;
    }
    if(copied!=wanted) return fail(e,"file range is not covered by parts");
    if(offset==0) {
        EVP_MD_CTX_free(file->sequential);file->sequential=EVP_MD_CTX_new();file->verified_until=0;
        if(!file->sequential || !EVP_DigestInit_ex(file->sequential,EVP_sha256(),NULL)) return fail(e,"cannot initialize sequential hash");
    }
    if(file->sequential && offset==file->verified_until) {
        if(!EVP_DigestUpdate(file->sequential,buffer,copied)) return fail(e,"cannot hash read range");
        file->verified_until+=copied;
        if(file->verified_until==file->attr.size) {
            unsigned char bytes[32];unsigned n;char hash[65];
            if(!EVP_DigestFinal_ex(file->sequential,bytes,&n) || n!=32) return fail(e,"cannot finalize file hash");
            EVP_MD_CTX_free(file->sequential);file->sequential=NULL;
            for(size_t i=0;i<32;i++) snprintf(hash+i*2,3,"%02x",bytes[i]);
            if(!same(hash,json_string(file->commit->caption,"file_sha256"))) {
                file->hash_failed=1;return fail(e,"whole-file SHA-256 mismatch");
            }
        }
    } else {EVP_MD_CTX_free(file->sequential);file->sequential=NULL;}
    *length=copied;return 0;
}

static int stream_commit(tgfs *fs,message *commit,FILE *output,tgfs_error *e)
{
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
        if (output && fwrite(bytes, 1, length, output) != length) {
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
    if (output && fflush(output)) { fail(e, "cannot flush file output"); goto done; }
    result = 0;
done:
    EVP_MD_CTX_free(full);
    return result;
}

int tgfs_cat(tgfs *fs, const char *path, FILE *output, tgfs_error *e)
{
    const char *id = resolve(fs, path, e);
    if (!id) return -1;
    if (directory_known(fs, id)) return fail_code(e, EISDIR, "cannot read a directory as a file");
    message *commit = selected_commit(fs, id, e);
    if (!commit) return -1;
    return stream_commit(fs,commit,output,e);
}

typedef struct {
    message *commit;
    uint64_t size;
    const char *hash;
    const char *operation;
} duplicate_commit;

static int duplicate_commit_order(const void *left,const void *right)
{
    const duplicate_commit *a=left,*b=right;
    int hash=strcmp(a->hash,b->hash);
    if(hash) return hash;
    if(a->size!=b->size) return a->size<b->size?-1:1;
    int operation=strcmp(a->operation,b->operation);
    return operation?operation:by_id(a->commit,b->commit);
}

static int same_parts(const message *a,const message *b)
{
    const cJSON *part_a=cJSON_GetObjectItemCaseSensitive(a->body,"parts")->child;
    const cJSON *part_b=cJSON_GetObjectItemCaseSensitive(b->body,"parts")->child;
    while(part_a && part_b) {
        uint64_t id_a,id_b;
        if(json_decimal(part_a,"message_id",&id_a) || json_decimal(part_b,"message_id",&id_b) || id_a!=id_b) return 0;
        part_a=part_a->next;
        part_b=part_b->next;
    }
    return !part_a && !part_b;
}

typedef struct {
    const char *id;
    message *latest;
    int directory;
    int reached;
} captured_object;

typedef struct {
    cJSON *catalog_entries;
    cJSON *referencing_commits;
    int reported_chunk;
} capture_references;

static int object_order(const void *left,const void *right)
{
    const captured_object *a=left,*b=right;
    return strcmp(a->id,b->id);
}

static captured_object *find_object(captured_object *objects,size_t count,const char *id)
{
    captured_object key={.id=id};
    return bsearch(&key,objects,count,sizeof(*objects),object_order);
}

static int add_report_id(cJSON *array,uint64_t id,tgfs_error *e)
{
    char text[32];snprintf(text,sizeof(text),"%" PRIu64,id);
    cJSON *value=cJSON_CreateString(text);
    if(!value || !cJSON_AddItemToArray(array,value)) {
        cJSON_Delete(value);
        return fail(e,"out of memory");
    }
    return 0;
}

/* The same directory can have more than one name. Visit each reachable directory once. */
static int report_catalog_refs(tgfs *fs,captured_object *objects,size_t count,capture_references *refs,tgfs_error *e)
{
    captured_object **queue=calloc(count,sizeof(*queue));
    if(!queue) return fail(e,"out of memory");
    captured_object *root=find_object(objects,count,fs->root_id);
    queue[0]=root;root->reached=1;
    size_t head=0,tail=1;
    int rc=-1;
    while(head<tail) {
        const char *parent=queue[head++]->id;
        const cJSON *entries=directory_entries(fs,parent,e),*entry;
        if(!entries) goto done;
        cJSON_ArrayForEach(entry,entries) {
            captured_object *child=find_object(objects,count,json_string(entry,"object_id"));
            if(!child) continue;
            if(child->directory) {
                if(!child->reached) {child->reached=1;queue[tail++]=child;}
                continue;
            }
            cJSON *array=child->latest?refs[child->latest-fs->messages].catalog_entries:NULL;
            if(!array) continue;
            cJSON *name=cJSON_CreateObject();
            if(!name || !cJSON_AddItemToArray(array,name)) {
                cJSON_Delete(name);fail(e,"out of memory");goto done;
            }
            if(!cJSON_AddStringToObject(name,"parent_object_id",parent) ||
                !cJSON_AddStringToObject(name,"name",json_string(entry,"name"))) {
                fail(e,"out of memory");goto done;
            }
        }
    }
    rc=0;
done:
    free(queue);
    return rc;
}

static int report_chunk_refs(tgfs *fs,capture_references *refs,tgfs_error *e)
{
    for(size_t i=0;i<fs->count;i++) {
        const message *commit=&fs->messages[i];
        if(!same(commit->type,"FILE_COMMIT")) continue;
        const cJSON *part;
        cJSON_ArrayForEach(part,cJSON_GetObjectItemCaseSensitive(commit->body,"parts")) {
            uint64_t id;
            if(json_decimal(part,"message_id",&id)) return fail(e,"invalid CHUNK reference");
            message *chunk=message_find(fs,id);
            cJSON *commits=chunk?refs[chunk-fs->messages].referencing_commits:NULL;
            if(commits && add_report_id(commits,commit->id,e))
                return -1;
        }
    }
    return 0;
}

static int report_verified_commit(cJSON *group,tgfs *fs,message *commit,capture_references *refs,tgfs_error *e)
{
    tgfs_error verify={0};
    if(stream_commit(fs,commit,NULL,&verify))
        return fail(e,"FILE_COMMIT %" PRIu64 ": %s",commit->id,verify.message);
    cJSON *entry=cJSON_CreateObject();
    if(!entry || !cJSON_AddItemToArray(group,entry)) {
        cJSON_Delete(entry);
        return fail(e,"out of memory");
    }
    char id[32];snprintf(id,sizeof(id),"%" PRIu64,commit->id);
    cJSON *parts=cJSON_AddArrayToObject(entry,"chunk_ids");
    refs[commit-fs->messages].catalog_entries=cJSON_AddArrayToObject(entry,"catalog_entries");
    if(!parts || !refs[commit-fs->messages].catalog_entries || !cJSON_AddStringToObject(entry,"commit_id",id) ||
        !cJSON_AddStringToObject(entry,"object_id",commit->object_id) ||
        !cJSON_AddStringToObject(entry,"version_id",json_string(commit->caption,"version_id")))
        return fail(e,"out of memory");
    const cJSON *part;
    cJSON_ArrayForEach(part,cJSON_GetObjectItemCaseSensitive(commit->body,"parts")) {
        uint64_t chunk_id;
        if(json_decimal(part,"message_id",&chunk_id) || add_report_id(parts,chunk_id,e)) return -1;
        message *chunk=message_find(fs,chunk_id); /* stream_commit already checked the reference. */
        refs[chunk-fs->messages].reported_chunk=1;
    }
    return 0;
}

static duplicate_commit *capture_commits(tgfs *fs,size_t *count,tgfs_error *e)
{
    duplicate_commit *commits=calloc(fs->count+1,sizeof(*commits));
    if(!commits) {fail(e,"out of memory");return NULL;}
    *count=0;
    for(size_t i=0;i<fs->count;i++) {
        message *commit=&fs->messages[i];
        if(!same(commit->type,"FILE_COMMIT")) continue;
        uint64_t size;
        if(json_decimal(commit->caption,"file_size",&size)) {
            free(commits);fail(e,"invalid FILE_COMMIT size");return NULL;
        }
        commits[(*count)++]=(duplicate_commit){commit,size,json_string(commit->caption,"file_sha256"),
            json_string(commit->caption,"operation_id")};
    }
    qsort(commits,*count,sizeof(*commits),duplicate_commit_order);
    return commits;
}

static int report_capture_catalog(tgfs *fs,const duplicate_commit *commits,size_t count,
                                  capture_references *refs,tgfs_error *e)
{
    captured_object *objects=calloc(fs->count+1,sizeof(*objects));
    if(!objects) return fail(e,"out of memory");
    objects[0]=(captured_object){.id=fs->root_id,.directory=1};
    for(size_t i=0;i<fs->count;i++) {
        const message *record=&fs->messages[i];
        objects[i+1]=(captured_object){.id=record->object_id,.directory=same(record->type,"DIRECTORY")};
    }
    qsort(objects,fs->count+1,sizeof(*objects),object_order);
    size_t object_count=0;
    for(size_t i=0;i<=fs->count;i++) {
        if(object_count && same(objects[object_count-1].id,objects[i].id))
            objects[object_count-1].directory |= objects[i].directory;
        else objects[object_count++]=objects[i];
    }
    for(size_t i=0;i<count;i++) {
        if(i && same(commits[i-1].operation,commits[i].operation)) continue;
        message *commit=commits[i].commit;
        captured_object *object=find_object(objects,object_count,commit->object_id);
        if(!object->latest || object->latest->id<commit->id) object->latest=commit;
    }
    int rc=report_catalog_refs(fs,objects,object_count,refs,e);
    free(objects);
    return rc;
}

int tgfs_duplicate_groups(tgfs *fs,cJSON *groups,cJSON *chunk_refs,tgfs_error *e)
{
    size_t count;
    duplicate_commit *commits=capture_commits(fs,&count,e);
    if(!commits) return -1;
    capture_references *refs=calloc(fs->count+1,sizeof(*refs));
    int result=-1;
    if(!refs) {fail(e,"out of memory");goto done;}
    for(size_t start=0;start<count;) {
        size_t end=start+1;
        int distinct_parts=0;
        while(end<count && commits[start].size==commits[end].size && same(commits[start].hash,commits[end].hash)) {
            if(!distinct_parts && !same_parts(commits[start].commit,commits[end].commit)) distinct_parts=1;
            end++;
        }
        if(distinct_parts) {
            cJSON *group=cJSON_CreateObject();
            if(!group || !cJSON_AddItemToArray(groups,group)) {cJSON_Delete(group);fail(e,"out of memory");goto done;}
            char size[32];snprintf(size,sizeof(size),"%" PRIu64,commits[start].size);
            cJSON *members=cJSON_AddArrayToObject(group,"verified_commits");
            if(!members || !cJSON_AddStringToObject(group,"file_sha256",commits[start].hash) ||
                !cJSON_AddStringToObject(group,"file_size",size)) {fail(e,"out of memory");goto done;}
            for(size_t i=start;i<end;i++) {
                /* A retry of the same operation points to the same CHUNKs. Verify the first publication. */
                if(i>start && same(commits[i-1].operation,commits[i].operation)) continue;
                if(report_verified_commit(members,fs,commits[i].commit,refs,e)) goto done;
            }
        }
        start=end;
    }
    if(groups->child) {
        for(size_t i=0;i<fs->count;i++) {
            if(!refs[i].reported_chunk) continue;
            cJSON *entry=cJSON_CreateObject();
            if(!entry || !cJSON_AddItemToArray(chunk_refs,entry)) {
                cJSON_Delete(entry);fail(e,"out of memory");goto done;
            }
            char id[32];snprintf(id,sizeof(id),"%" PRIu64,fs->messages[i].id);
            refs[i].referencing_commits=cJSON_AddArrayToObject(entry,"commit_ids");
            if(!refs[i].referencing_commits || !cJSON_AddStringToObject(entry,"chunk_id",id)) {fail(e,"out of memory");goto done;}
        }
        if(report_capture_catalog(fs,commits,count,refs,e) || report_chunk_refs(fs,refs,e)) goto done;
    }
    result=0;
done:
    free(refs);free(commits);
    return result;
}

static int chunk_id_order(const void *left,const void *right)
{
    uint64_t a=*(const uint64_t *)left,b=*(const uint64_t *)right;
    return (a>b)-(a<b);
}

int tgfs_retained_capture_refs(tgfs *fs,const cJSON *chunk_refs,cJSON *retained,tgfs_error *e)
{
    size_t count=(size_t)cJSON_GetArraySize(chunk_refs);
    if(!count) return 0;
    uint64_t *ids=calloc(count,sizeof(*ids));
    capture_references *refs=calloc(fs->count+1,sizeof(*refs));
    duplicate_commit *commits=NULL;
    int rc=-1;
    if(!ids || !refs) {fail(e,"out of memory");goto done;}
    const cJSON *ref;
    size_t index=0;
    cJSON_ArrayForEach(ref,chunk_refs) {
        if(json_decimal(ref,"chunk_id",&ids[index++])) {fail(e,"invalid reported chunk ID");goto done;}
    }
    /* chunk_references is emitted in source message order. */
    for(size_t i=0;i<fs->count;i++) {
        message *commit=&fs->messages[i];
        if(!same(commit->type,"FILE_COMMIT")) continue;
        cJSON *parts=NULL;
        const cJSON *part;
        cJSON_ArrayForEach(part,cJSON_GetObjectItemCaseSensitive(commit->body,"parts")) {
            uint64_t id;
            if(json_decimal(part,"message_id",&id)) {fail(e,"invalid retained CHUNK reference");goto done;}
            if(!bsearch(&id,ids,count,sizeof(*ids),chunk_id_order)) continue;
            if(!parts) {
                cJSON *entry=cJSON_CreateObject();
                if(!entry || !cJSON_AddItemToArray(retained,entry)) {
                    cJSON_Delete(entry);fail(e,"out of memory");goto done;
                }
                char mid[32];snprintf(mid,sizeof(mid),"%" PRIu64,commit->id);
                parts=cJSON_AddArrayToObject(entry,"chunk_ids");
                refs[i].catalog_entries=cJSON_AddArrayToObject(entry,"catalog_entries");
                if(!parts || !refs[i].catalog_entries || !cJSON_AddStringToObject(entry,"commit_id",mid)) {
                    fail(e,"out of memory");goto done;
                }
            }
            if(add_report_id(parts,id,e)) goto done;
        }
    }
    if(retained->child) {
        size_t commit_count;
        commits=capture_commits(fs,&commit_count,e);
        if(!commits || report_capture_catalog(fs,commits,commit_count,refs,e)) goto done;
    }
    rc=0;
done:
    free(commits);free(refs);free(ids);
    return rc;
}
