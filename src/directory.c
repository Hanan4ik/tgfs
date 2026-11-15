#include "internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const cJSON *value;
    const char *directory, *operation_id, *name, *action;
    int removed;
} operation;
static int same(const char *a, const char *b) { return a && b && !strcmp(a,b); }

int directory_operations_valid(const cJSON *body, tgfs_error *e)
{
    const cJSON *ops=cJSON_GetObjectItemCaseSensitive(body,"operations");
    if(cJSON_HasObjectItem(body,"entries") || !cJSON_IsArray(ops) || cJSON_GetArraySize(ops)>TGFS_MAX_RECORDS)
        return fail(e,"invalid DIRECTORY operations document");
    const cJSON *op;
    cJSON_ArrayForEach(op,ops) {
        const char *action=json_string(op,"action");
        if(!cJSON_IsObject(op) || !uuid_valid(json_string(op,"operation_id")) || !name_valid(json_string(op,"name")))
            return fail(e,"invalid DIRECTORY operation identity or name");
        if(same(action,"ADD")) {
            if(!uuid_valid(json_string(op,"object_id")) || cJSON_HasObjectItem(op,"observed_adds"))
                return fail(e,"invalid DIRECTORY ADD");
        } else if(same(action,"REMOVE")) {
            const cJSON *refs=cJSON_GetObjectItemCaseSensitive(op,"observed_adds");
            if(cJSON_HasObjectItem(op,"object_id") || !cJSON_IsArray(refs) ||
                !cJSON_GetArraySize(refs) || cJSON_GetArraySize(refs)>TGFS_MAX_RECORDS)
                return fail(e,"invalid DIRECTORY REMOVE");
            const cJSON *ref,*other;
            cJSON_ArrayForEach(ref,refs) {
                if(!cJSON_IsString(ref) || !uuid_valid(ref->valuestring)) return fail(e,"invalid observed ADD ID");
                for(other=ref->next;other;other=other->next)
                    if(cJSON_IsString(other) && same(ref->valuestring,other->valuestring))
                        return fail(e,"duplicate observed ADD ID");
            }
        } else return fail(e,"unsupported DIRECTORY operation action");
    }
    return 0;
}

static cJSON *merge_select(const tgfs_directory_batch *batches,size_t count,const char *directory,
                           const char *name,const char *target,cJSON *observed,tgfs_error *e)
{
    operation *ops=calloc(TGFS_MAX_RECORDS,sizeof(*ops));
    cJSON *views=cJSON_CreateObject();
    size_t n=0;
    if(!ops || !views) {fail(e,"out of memory merging directories");goto invalid;}
    for(size_t i=0;i<count;i++) {
        const tgfs_directory_batch *b=&batches[i];
        if(!cJSON_HasObjectItem(b->body,"operations")) continue;
        if(directory_operations_valid(b->body,e)) goto invalid;
        if(!cJSON_HasObjectItem(views,b->object_id)) {
            cJSON *entries=cJSON_CreateArray();
            if(!entries || !cJSON_AddItemToObject(views,b->object_id,entries)) {
                cJSON_Delete(entries);fail(e,"out of memory merging directories");goto invalid;
            }
        }
        const cJSON *value;
        cJSON_ArrayForEach(value,cJSON_GetObjectItemCaseSensitive(b->body,"operations")) {
            const char *id=json_string(value,"operation_id");
            size_t j;
            for(j=0;j<n;j++) if(same(ops[j].operation_id,id)) break;
            if(j<n) {
                if(!same(ops[j].directory,b->object_id) || !cJSON_Compare(ops[j].value,value,1)) {
                    fail(e,"DIRECTORY operation_id reused with different contents or scope");goto invalid;
                }
                continue; /* Retry does not resurrect an ADD or refresh its order. */
            }
            if(n==TGFS_MAX_RECORDS) {fail(e,"too many DIRECTORY operations");goto invalid;}
            operation *op=&ops[n];
            *op=(operation){.value=value,.directory=b->object_id,.operation_id=id,
                .name=json_string(value,"name"),.action=json_string(value,"action")};
            if(same(op->action,"REMOVE")) {
                const cJSON *ref;
                cJSON_ArrayForEach(ref,cJSON_GetObjectItemCaseSensitive(value,"observed_adds")) {
                    for(j=0;j<n;j++) if(same(ops[j].operation_id,ref->valuestring)) break;
                    if(j==n || !same(ops[j].action,"ADD") || !same(ops[j].directory,op->directory) ||
                        !same(ops[j].name,op->name)) {
                        fail(e,"REMOVE refers to an unavailable or mismatching observed ADD");goto invalid;
                    }
                    ops[j].removed=1;
                }
            }
            n++;
        }
    }
    for(size_t i=0;i<n;i++) {
        operation *op=&ops[i];
        if(op->removed || !same(op->action,"ADD")) continue;
        cJSON *entries=cJSON_GetObjectItemCaseSensitive(views,op->directory);
        const char *object=json_string(op->value,"object_id");
        if(observed && same(op->directory,directory) && same(op->name,name) && same(object,target)) {
            cJSON *ref=cJSON_CreateString(op->operation_id);
            if(!ref || !cJSON_AddItemToArray(observed,ref)) {
                cJSON_Delete(ref);fail(e,"out of memory collecting observed ADDs");goto invalid;
            }
        }
        const cJSON *entry;
        cJSON_ArrayForEach(entry,entries) if(same(json_string(entry,"name"),op->name)) break;
        if(entry) {
            if(!same(json_string(entry,"object_id"),object)) {
                fail(e,"DIRECTORY name conflict: %s",op->name);goto invalid;
            }
            continue;
        }
        cJSON *added=cJSON_CreateObject();
        if(!added || !cJSON_AddStringToObject(added,"name",op->name) ||
            !cJSON_AddStringToObject(added,"object_id",object) || !cJSON_AddItemToArray(entries,added)) {
            cJSON_Delete(added);fail(e,"out of memory merging entries");goto invalid;
        }
    }
    free(ops);return views;
invalid:
    free(ops);cJSON_Delete(views);return NULL;
}

cJSON *directory_merge(const tgfs_directory_batch *batches,size_t count,tgfs_error *e)
{
    return merge_select(batches,count,NULL,NULL,NULL,NULL,e);
}
cJSON *directory_observed_adds(const tgfs_directory_batch *batches,size_t count,const char *directory,
                               const char *name,const char *object,tgfs_error *e)
{
    cJSON *refs=cJSON_CreateArray();
    if(!refs) {fail(e,"out of memory");return NULL;}
    cJSON *views=merge_select(batches,count,directory,name,object,refs,e);
    if(!views) {cJSON_Delete(refs);return NULL;}
    cJSON_Delete(views);
    if(!cJSON_GetArraySize(refs)) {cJSON_Delete(refs);fail(e,"unlink requires live DIRECTORY ADD IDs; legacy entries are unsupported");return NULL;}
    return refs;
}
