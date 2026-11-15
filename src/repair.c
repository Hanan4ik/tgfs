#include "internal.h"
#include "tgfs/bot_api.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int same(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static int log_event(int fd,const char *event,const char *id,tgfs_error *e)
{
    char line[160];int n=snprintf(line,sizeof(line),"{\"event\":\"%s\",\"message_id\":\"%s\"}\n",event,id?id:"");
    if(n<0 || (size_t)n>=sizeof(line)) return fail(e,"repair log overflow");
    size_t done=0;while(done<(size_t)n) {ssize_t wrote=write(fd,line+done,(size_t)n-done);if(wrote<0 && errno==EINTR) continue;if(wrote<=0) return fail(e,"cannot write repair log");done+=(size_t)wrote;}
    return fsync(fd)?fail(e,"cannot persist repair log"):0;
}
static cJSON *params(const char *chat)
{
    cJSON *p=cJSON_CreateObject();if(p && !cJSON_AddStringToObject(p,"chat_id",chat)) {cJSON_Delete(p);p=NULL;}return p;
}
static int call(tgfs_bot *bot,const char *method,cJSON *p,cJSON **out,int mutation,tgfs_error *e)
{
    if(!p) return fail(e,"out of memory");
#ifndef TGFS_TESTING
    if(mutation) {struct timespec delay={1,100000000};while(nanosleep(&delay,&delay)) if(errno!=EINTR) {cJSON_Delete(p);return fail(e,"repair pacing failed");}}
#else
    (void)mutation;
#endif
    tgfs_bot_error b;int rc=tgfs_bot_call(bot,method,p,out,&b);cJSON_Delete(p);
    return rc?fail(e,"%s [http=%ld api=%d outcome_unknown=%d]",b.message,b.http_status,b.api_code,b.outcome_unknown):0;
}
static const cJSON *record_find(const cJSON *journal,const char *id)
{
    const cJSON *r;cJSON_ArrayForEach(r,cJSON_GetObjectItemCaseSensitive(journal,"records")) if(same(json_string(r,"message_id"),id)) return r;return NULL;
}
static int message_matches(const cJSON *message,const cJSON *record,const char *chat,const char *id,tgfs_error *e)
{
    long long mid,cid,expected;uint64_t wanted;
    const cJSON *c=cJSON_GetObjectItemCaseSensitive(message,"chat");
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(message,"document"),*b=cJSON_GetObjectItemCaseSensitive(record,"document");
    if(tgfs_bot_integer(message,"message_id",&mid) || decimal(id,&wanted) || mid!=(long long)wanted ||
        tgfs_bot_channel_id(chat,&expected) || tgfs_bot_integer(c,"id",&cid) || cid!=expected ||
        !same(json_string(c,"type"),"channel") || !same(json_string(a,"file_id"),json_string(b,"file_id")) ||
        !same(json_string(a,"file_name"),json_string(b,"file_name")) ||
        !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(a,"file_size"),cJSON_GetObjectItemCaseSensitive(b,"file_size"),1))
        return fail(e,"fresh message/document disagrees with capture");
    return 0;
}
static cJSON *fresh_caption(tgfs_bot *bot,const cJSON *journal,const cJSON *record,int fd,tgfs_error *e)
{
    const char *storage=json_string(journal,"storage_chat_id"),*service=json_string(journal,"service_chat_id"),*id=json_string(record,"message_id");
    uint64_t mid;long long service_id,storage_id,copy_id=0,origin_id,origin_chat,copy_chat;
    if(decimal(id,&mid) || tgfs_bot_channel_id(service,&service_id) || tgfs_bot_channel_id(storage,&storage_id) || service_id==storage_id) {fail(e,"invalid repair channels");return NULL;}
    if(log_event(fd,"service_channel",service,e)) return NULL;
    cJSON *p=params(service),*out=NULL,*caption=NULL;
    if(!p || !cJSON_AddStringToObject(p,"from_chat_id",storage) || !cJSON_AddNumberToObject(p,"message_id",(double)mid)) {cJSON_Delete(p);fail(e,"out of memory");return NULL;}
    if(log_event(fd,"forward_pending",id,e)) {cJSON_Delete(p);return NULL;}
    if(call(bot,"forwardMessage",p,&out,1,e)) goto done;
    const cJSON *origin=cJSON_GetObjectItemCaseSensitive(out,"forward_origin");
    if(tgfs_bot_integer(out,"message_id",&copy_id) || copy_id<=0 ||
        tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(out,"chat"),"id",&copy_chat) || copy_chat!=service_id) {fail(e,"repair forward returned unknown copy identity");copy_id=0;goto done;}
    char copy[32];snprintf(copy,sizeof(copy),"%lld",copy_id);
    if(log_event(fd,"service_copy_known",copy,e)) goto done;
    if(!same(json_string(origin,"type"),"channel") || tgfs_bot_integer(origin,"message_id",&origin_id) || origin_id!=(long long)mid ||
        tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(origin,"chat"),"id",&origin_chat) || origin_chat!=storage_id) {fail(e,"repair forward origin mismatch");goto cleanup;}
    const char *raw=json_string(out,"caption");
    if(raw) caption=json_parse_bytes((const unsigned char *)raw,strlen(raw),e);else fail(e,"fresh caption missing");
    /* Forwarded Message has a service identity; compare the immutable document separately. */
    cJSON *source=cJSON_Duplicate(out,1);
    if(!source) {cJSON_Delete(caption);caption=NULL;fail(e,"out of memory");goto cleanup;}
    cJSON_ReplaceItemInObjectCaseSensitive(source,"message_id",cJSON_CreateNumber((double)mid));
    cJSON *chat=cJSON_GetObjectItemCaseSensitive(source,"chat");
    cJSON_ReplaceItemInObjectCaseSensitive(chat,"id",cJSON_CreateNumber((double)storage_id));
    if(message_matches(source,record,storage,id,e)) {cJSON_Delete(caption);caption=NULL;}
    cJSON_Delete(source);
cleanup:
    cJSON_Delete(out);out=NULL;p=params(service);
    if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)copy_id)) {cJSON_Delete(p);cJSON_Delete(caption);caption=NULL;fail(e,"out of memory");goto done;}
    if(call(bot,"deleteMessage",p,&out,1,e) || !cJSON_IsTrue(out) || log_event(fd,"service_copy_deleted",copy,e)) {cJSON_Delete(caption);caption=NULL;fail(e,"service cleanup failed; consult repair log");}
done:
    cJSON_Delete(out);return caption;
}
int repair_backlinks(tgfs_bot *bot,const cJSON *journal,const cJSON *intent,cJSON *report,int fd,tgfs_error *e)
{
    const char *bad[]={"unobserved_part_indexes","ambiguous_part_indexes","unobserved_known_ids"};
    const cJSON *ids=cJSON_GetObjectItemCaseSensitive(report,"commit_ids");
    if(!same(json_string(intent,"kind"),"put") || !same(json_string(report,"staging"),"verified") || !cJSON_GetArraySize(ids)) return fail(e,"repair requires a verified put and confirmed FILE_COMMIT");
    for(size_t i=0;i<3;i++) if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,bad[i]))) return fail(e,"repair blocked by missing or ambiguous parts");
    cJSON *applied=cJSON_AddArrayToObject(report,"repaired_chunk_ids"),*skipped=cJSON_AddArrayToObject(report,"repair_skipped_ids");
    cJSON *live=cJSON_CreateArray();int rc=-1;
    if(!applied || !skipped || !live) {fail(e,"out of memory");goto done;}
    const cJSON *fixes=cJSON_GetObjectItemCaseSensitive(report,"backlink_repairs");
    if(!cJSON_GetArraySize(fixes)) {rc=0;goto done;}
    cJSON *p=params(json_string(journal,"storage_chat_id")),*out=NULL;
    if(call(bot,"getChat",p,&out,0,e)) goto done;
    const cJSON *pin=cJSON_GetObjectItemCaseSensitive(out,"pinned_message");const char *raw=json_string(pin,"text");
    cJSON *header=raw?json_parse_bytes((const unsigned char *)raw,strlen(raw),e):NULL;
    uint64_t start;long long pinned,expected_channel,actual_channel,pin_channel;
    int valid=header && !tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&expected_channel) &&
        !tgfs_bot_integer(out,"id",&actual_channel) && actual_channel==expected_channel && same(json_string(out,"type"),"channel") &&
        !tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(pin,"chat"),"id",&pin_channel) && pin_channel==expected_channel && !json_decimal(cJSON_GetObjectItemCaseSensitive(journal,"superblock"),"start_id",&start) &&
        !tgfs_bot_integer(pin,"message_id",&pinned) && pinned==(long long)start &&
        json_semantic_equal(header,cJSON_GetObjectItemCaseSensitive(journal,"superblock"));
    cJSON_Delete(header);cJSON_Delete(out);
    if(!valid) {fail(e,"pinned SUPERBLOCK differs from capture");goto done;}
    const char *commit_id=cJSON_GetArrayItem(ids,0)->valuestring;
    const cJSON *commit_record=record_find(journal,commit_id);
    cJSON *commit=fresh_caption(bot,journal,commit_record,fd,e);
    if(!commit) goto done;
    valid=json_semantic_equal(commit,cJSON_GetObjectItemCaseSensitive(commit_record,"record"));cJSON_Delete(commit);
    if(!valid) {fail(e,"fresh FILE_COMMIT differs from capture");goto done;}
    const cJSON *fix;
    /* Preflight every planned message before the first storage edit. */
    cJSON_ArrayForEach(fix,fixes) {
        const cJSON *record=record_find(journal,json_string(fix,"message_id"));
        cJSON *caption=fresh_caption(bot,journal,record,fd,e);if(!caption) goto done;
        const char *back=json_string(caption,"file_commit_id");
        if(!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(caption,"file_commit_id")) && !same(back,commit_id)) {cJSON_Delete(caption);fail(e,"fresh CHUNK backlink conflicts with canonical commit");goto done;}
        cJSON *normalized=cJSON_Duplicate(caption,1),*captured=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(record,"record"),1);
        if(!normalized || !captured) {cJSON_Delete(normalized);cJSON_Delete(captured);cJSON_Delete(caption);fail(e,"out of memory");goto done;}
        cJSON_DeleteItemFromObjectCaseSensitive(normalized,"file_commit_id");cJSON_DeleteItemFromObjectCaseSensitive(captured,"file_commit_id");
        valid=json_semantic_equal(normalized,captured);cJSON_Delete(normalized);cJSON_Delete(captured);
        if(!valid || !cJSON_AddItemToArray(live,caption)) {cJSON_Delete(caption);fail(e,"fresh CHUNK metadata differs from capture");goto done;}
    }
    int index=0;
    cJSON_ArrayForEach(fix,fixes) {
        cJSON *caption=cJSON_GetArrayItem(live,index++);const char *id=json_string(fix,"message_id");
        cJSON *v=cJSON_CreateString(id);
        if(!v) {fail(e,"out of memory");goto done;}
        if(same(json_string(caption,"file_commit_id"),commit_id) && same(json_string(caption,"next_message"),json_string(fix,"next_message"))) {
            if(!cJSON_AddItemToArray(skipped,v)) {cJSON_Delete(v);fail(e,"out of memory");goto done;}continue;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(caption,"file_commit_id");cJSON_DeleteItemFromObjectCaseSensitive(caption,"next_message");
        if(!cJSON_AddStringToObject(caption,"file_commit_id",commit_id) || !cJSON_AddStringToObject(caption,"next_message",json_string(fix,"next_message"))) {cJSON_Delete(v);fail(e,"out of memory");goto done;}
        char *text=cJSON_PrintUnformatted(caption);uint64_t mid;decimal(id,&mid);p=params(json_string(journal,"storage_chat_id"));out=NULL;
        if(!text || !p || !cJSON_AddNumberToObject(p,"message_id",(double)mid) || !cJSON_AddStringToObject(p,"caption",text)) {free(text);cJSON_Delete(p);cJSON_Delete(v);fail(e,"out of memory");goto done;}
        if(log_event(fd,"edit_pending",id,e)) {free(text);cJSON_Delete(p);cJSON_Delete(v);goto done;}
        int edited=call(bot,"editMessageCaption",p,&out,1,e);
        if(!edited && (message_matches(out,record_find(journal,id),json_string(journal,"storage_chat_id"),id,e) || !same(json_string(out,"caption"),text))) edited=fail(e,"repair edit response mismatch; outcome may be unknown");
        free(text);cJSON_Delete(out);
        if(edited || log_event(fd,"edit_confirmed",id,e)) {cJSON_Delete(v);goto done;}
        if(!cJSON_AddItemToArray(applied,v)) {cJSON_Delete(v);fail(e,"out of memory");goto done;}
    }
    rc=0;
done:
    cJSON_Delete(live);
    if(!rc) {
        if(!cJSON_AddBoolToObject(report,"repair_applied",1)) return fail(e,"out of memory");
        if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"backlink_repairs"))) {
            cJSON *step=cJSON_CreateString("capture_after_repair");
            if(!step || !cJSON_ReplaceItemInObjectCaseSensitive(report,"next_step",step)) {cJSON_Delete(step);return fail(e,"out of memory");}
        }
    }
    return rc;
}
