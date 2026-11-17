#include "internal.h"
#include "tgfs/bot_api.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

static int same(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static int append_log(int fd,const char *line,size_t length,tgfs_error *e)
{
    size_t done=0;while(done<length) {ssize_t wrote=write(fd,line+done,length-done);if(wrote<0 && errno==EINTR) continue;if(wrote<=0) return fail(e,"cannot write repair log");done+=(size_t)wrote;}
    return fsync(fd)?fail(e,"cannot persist repair log"):0;
}
static int log_event(int fd,const char *event,const char *id,tgfs_error *e)
{
    char line[160];int n=snprintf(line,sizeof(line),"{\"event\":\"%s\",\"message_id\":\"%s\"}\n",event,id?id:"");
    if(n<0 || (size_t)n>=sizeof(line)) return fail(e,"repair log overflow");
    return append_log(fd,line,(size_t)n,e);
}
/* Only a validated, explicit rejection may release this request's send barrier. */
int upload_rejection(int fd,const char *event,const char *id,const char *operation,const tgfs_bot_error *b,tgfs_error *e)
{
    if(b->kind!=TGFS_BOT_API || b->outcome_unknown || b->http_status<400 || b->http_status>=500 ||
        b->api_code!=b->http_status || (b->api_code==429 && !b->retry_after)) return 0;
    time_t now=time(NULL);if(now<0) return fail(e,"cannot timestamp rejected request; send barrier retained");
    uint64_t deadline=b->retry_after?(uint64_t)now+b->retry_after:0;
    char line[320];int n=snprintf(line,sizeof(line),"{\"event\":\"%s\",\"message_id\":\"%s\",\"operation_id\":\"%s\",\"http_status\":%ld,\"api_code\":%d,\"retry_not_before\":\"%llu\"}\n",
        event,id,operation,b->http_status,b->api_code,(unsigned long long)deadline);
    if(n<0 || (size_t)n>=sizeof(line)) return fail(e,"rejection log overflow");
    return append_log(fd,line,(size_t)n,e);
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
static unsigned char *document_bytes(tgfs_bot *bot,const char *id,size_t expected,size_t *size,tgfs_error *e)
{
    cJSON *p=cJSON_CreateObject(),*out=NULL;
    if(!p || !cJSON_AddStringToObject(p,"file_id",id)) {cJSON_Delete(p);fail(e,"out of memory");return NULL;}
    if(call(bot,"getFile",p,&out,0,e)) return NULL;
    const char *path=json_string(out,"file_path");long long length;
    if(!path || !json_string(out,"file_id") || tgfs_bot_integer(out,"file_size",&length) || length!=(long long)expected) {
        cJSON_Delete(out);fail(e,"fresh document getFile metadata differs");return NULL;
    }
    unsigned char *bytes=NULL;tgfs_bot_error b;
    int rc=tgfs_bot_download(bot,path,&bytes,size,&b);cJSON_Delete(out);
    if(rc) {fail(e,"%s [http=%ld api=%d]",b.message,b.http_status,b.api_code);return NULL;}
    if(*size!=expected) {free(bytes);fail(e,"fresh document download size differs");return NULL;}
    return bytes;
}
static int message_matches(tgfs_bot *bot,const cJSON *message,const cJSON *record,const char *chat,const char *id,tgfs_error *e)
{
    long long mid,cid,expected,asize,bsize;uint64_t wanted;
    const cJSON *c=cJSON_GetObjectItemCaseSensitive(message,"chat");
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(message,"document"),*b=cJSON_GetObjectItemCaseSensitive(record,"document");
    if(tgfs_bot_integer(message,"message_id",&mid) || decimal(id,&wanted) || mid!=(long long)wanted)
        return fail(e,"fresh message_id differs for expected message %s",id);
    if(tgfs_bot_channel_id(chat,&expected) || tgfs_bot_integer(c,"id",&cid) || cid!=expected || !same(json_string(c,"type"),"channel"))
        return fail(e,"fresh chat identity differs for message %s",id);
    if(!same(json_string(a,"file_name"),json_string(b,"file_name"))) return fail(e,"fresh document.file_name differs for message %s",id);
    if(tgfs_bot_integer(a,"file_size",&asize) || tgfs_bot_integer(b,"file_size",&bsize) || asize<0 || asize!=bsize || asize>TGFS_MAX_CHUNK)
        return fail(e,"fresh document.file_size differs for message %s",id);
    const char *aid=json_string(a,"file_id"),*bid=json_string(b,"file_id");
    const char *au=json_string(a,"file_unique_id"),*bu=json_string(b,"file_unique_id");
    if(!aid || !*aid || !bid || !*bid) return fail(e,"fresh document.file_id missing for message %s",id);
    if(au && bu && !same(au,bu)) return fail(e,"fresh document.file_unique_id differs for message %s",id);
    if(!same(aid,bid)) {
        size_t an=0,bn=0;unsigned char *ab=document_bytes(bot,aid,(size_t)asize,&an,e);
        if(!ab) return -1;
        unsigned char *bb=document_bytes(bot,bid,(size_t)bsize,&bn,e);
        if(!bb) {free(ab);return -1;}
        int equal=an==bn && !memcmp(ab,bb,an);free(ab);free(bb);
        if(!equal) return fail(e,"fresh document bytes differ for message %s",id);
    }
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
    if(message_matches(bot,out,record,service,copy,e)) {cJSON_Delete(caption);caption=NULL;}

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
        if(!edited) {
            if(message_matches(bot,out,record_find(journal,id),json_string(journal,"storage_chat_id"),id,e)) edited=-1;
            else if(!same(json_string(out,"caption"),text)) edited=fail(e,"repair edit caption mismatch; outcome may be unknown");
        }
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

/* A durable pending record is a send barrier, even after a lost response or crash. */
static int directory_attempted(int fd,const char *operation,tgfs_error *e)
{
    struct stat st;
    if(fstat(fd,&st) || st.st_size>TGFS_MAX_JSON) return fail(e,"recovery log too large or unreadable");
    if(lseek(fd,0,SEEK_SET)<0) return fail(e,"cannot read recovery log");
    FILE *stream=fdopen(dup(fd),"r");if(!stream) return fail(e,"cannot inspect recovery log");
    char *line=NULL;size_t capacity=0,total=0;ssize_t n;int found=0;uint64_t deadline=0;
    while((n=getline(&line,&capacity,stream))>=0) {
        total+=(size_t)n;
        if(total>TGFS_MAX_JSON) {fail(e,"recovery log too large");found=-1;break;}
        cJSON *entry=json_parse_bytes((const unsigned char *)line,(size_t)n,e);
        if(!entry) {found=-1;break;}
        const char *event=json_string(entry,"event");
        if(same(event,"directory_send_pending")) {
            if(found || !same(json_string(entry,"message_id"),operation)) {cJSON_Delete(entry);fail(e,"conflicting ADD attempts in recovery log");found=-1;break;}
            found=1;
        }
        if(same(event,"directory_publication_confirmed")) found=2;
        if(same(event,"directory_send_rejected")) {
            size_t http,api;uint64_t when;
            if(found!=1 || !same(json_string(entry,"message_id"),operation) || json_count(entry,"http_status",&http) ||
                json_count(entry,"api_code",&api) || http<400 || http>=500 || api!=http ||
                json_decimal(entry,"retry_not_before",&when) || (api==429 && !when)) {
                cJSON_Delete(entry);fail(e,"invalid ADD rejection evidence in recovery log");found=-1;break;
            }
            found=0;if(when>deadline) deadline=when;
        }
        cJSON_Delete(entry);
    }
    if(ferror(stream)) {fail(e,"cannot read recovery log");found=-1;}
    free(line);fclose(stream);
    if(!found && deadline) {
        time_t now=time(NULL);
        if(now<0 || (uint64_t)now<=deadline) return fail(e,"ADD retry cooldown active; retry after Unix time %llu",(unsigned long long)deadline);
    }
    return found;
}
int resume_directory(tgfs_bot *bot,tgfs *fs,const cJSON *journal,const cJSON *intent,cJSON *report,int fd,tgfs_error *e)
{
    int mkdir=same(json_string(intent,"kind"),"mkdir");
    if(!mkdir && (!same(json_string(intent,"kind"),"put") || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(intent,"replace")) ||
        !cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"commit_ids")) || !same(json_string(report,"staging"),"verified")))
        return fail(e,"directory resume requires a verified new-file put with confirmed FILE_COMMIT");
    if(mkdir && !cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"child_ids")))
        return fail(e,"mkdir resume requires a confirmed child DIRECTORY");
    const char *blocked[]={"unobserved_part_indexes","ambiguous_part_indexes","unobserved_known_ids"};
    for(size_t i=0;i<3;i++) if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,blocked[i]))) return fail(e,"directory resume blocked by missing or ambiguous parts");
    if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,"directory_ids")))
        return cJSON_AddStringToObject(report,"directory_resume","already_observed")?0:fail(e,"out of memory");
    const char *phase=json_string(intent,"phase");
    if(mkdir ? (!same(phase,"child_sent") && !same(phase,"child_send_pending") && !same(phase,"child_response_uncertain")) :
        (!same(phase,"backlinks_pending") && !same(phase,"commit_send_pending") && !same(phase,"commit_response_uncertain")))
        return fail(e,"ADD may already have been attempted; directory resume is blocked");
    int attempted=directory_attempted(fd,json_string(intent,"add_operation_id"),e);if(attempted<0) return -1;
    if(attempted) return fail(e,"recovery log contains a previous ADD attempt; use a newer capture, never resend blindly");
    const char *path=json_string(intent,"parent_path"),*name=json_string(intent,"name"),*operation=json_string(intent,"add_operation_id");
    char mkdir_path[4097];
    if(mkdir) {
        const char *saved=json_string(intent,"path");
        if(!saved || saved[0]!='/' || strlen(saved)>4096) return fail(e,"invalid mkdir intent path");
        memcpy(mkdir_path,saved,strlen(saved)+1);size_t length=strlen(mkdir_path);
        while(length>1 && mkdir_path[length-1]=='/') mkdir_path[--length]=0;
        char *slash=strrchr(mkdir_path,'/');name=slash+1;path="/";
        if(slash!=mkdir_path) {*slash=0;path=mkdir_path;}
    }
    if(!path || !uuid_valid(operation)) return fail(e,"missing saved directory destination");
    char parent[37],grandparent[37];
    if(directory_target(fs,path,name,parent,grandparent,e)) return -1;
    const cJSON *body=cJSON_GetObjectItemCaseSensitive(intent,"directory_document"),*caption=cJSON_GetObjectItemCaseSensitive(intent,"directory_caption");
    const cJSON *ops=cJSON_GetObjectItemCaseSensitive(body,"operations"),*add=cJSON_GetArrayItem(ops,0);
    if(directory_operations_valid(body,e)) return -1;
    size_t format;
    if(json_count(caption,"format_version",&format) || format!=1 || !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(caption,"next_message")) ||
        cJSON_GetArraySize(ops)!=1 || !same(json_string(add,"action"),"ADD") || !same(json_string(add,"name"),name) ||
        !same(json_string(add,"operation_id"),operation) || !same(json_string(add,"object_id"),json_string(intent,"object_id")) ||
        !same(parent,json_string(intent,"parent_id")) || !same(json_string(caption,"object_id"),parent) ||
        !same(json_string(caption,"type"),"DIRECTORY") || !same(json_string(caption,"magic"),"TGFS") ||
        (grandparent[0]?!same(json_string(caption,"parent_id"),grandparent):!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(caption,"parent_id"))))
        return fail(e,"saved ADD/caption destination disagrees with intent and captured parent");
    const char *source_id;
    if(mkdir) {
        const cJSON *children=cJSON_GetObjectItemCaseSensitive(report,"child_ids");
        if(cJSON_GetArraySize(children)!=1) return fail(e,"mkdir resume requires one confirmed child DIRECTORY");
        source_id=cJSON_GetArrayItem(children,0)->valuestring;
        const char *known=json_string(intent,"child_message_id");
        if((cJSON_GetObjectItemCaseSensitive(intent,"child_message_id") && !same(known,source_id)) ||
            cJSON_GetObjectItemCaseSensitive(intent,"directory_message_id"))
            return fail(e,"saved mkdir message IDs disagree with captured pre-ADD state");
        const cJSON *cc=cJSON_GetObjectItemCaseSensitive(intent,"child_caption");
        const cJSON *cb=cJSON_GetObjectItemCaseSensitive(intent,"child_document");
        if(!same(json_string(cc,"object_id"),json_string(intent,"object_id")) ||
            !same(json_string(cc,"parent_id"),parent) || !same(json_string(cc,"magic"),"TGFS") ||
            !same(json_string(cc,"type"),"DIRECTORY") || json_count(cc,"format_version",&format) || format!=1 ||
            !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cc,"next_message")) || directory_operations_valid(cb,e) ||
            cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cb,"operations")))
            return fail(e,"saved child DIRECTORY disagrees with mkdir intent");
    } else {
        if(repair_backlinks(bot,journal,intent,report,fd,e)) return -1;
        source_id=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report,"commit_ids"),0)->valuestring;
    }
    const cJSON *record=record_find(journal,source_id);
    cJSON *fresh=fresh_caption(bot,journal,record,fd,e);if(!fresh) return -1;
    int good=json_semantic_equal(fresh,cJSON_GetObjectItemCaseSensitive(record,"record")) &&
        (!mkdir || cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fresh,"next_message")));cJSON_Delete(fresh);
    if(!good) return fail(e,"fresh source metadata or navigation differs from capture");
    /* Validate the pin again immediately before publication. */
    cJSON *p=params(json_string(journal,"storage_chat_id")),*out=NULL;
    if(call(bot,"getChat",p,&out,0,e)) return -1;
    const cJSON *pin=cJSON_GetObjectItemCaseSensitive(out,"pinned_message");const char *raw=json_string(pin,"text");
    cJSON *header=raw?json_parse_bytes((const unsigned char *)raw,strlen(raw),e):NULL;
    uint64_t start;long long mid,expected_channel,actual_channel,pin_channel;
    good=header && !tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&expected_channel) &&
        !tgfs_bot_integer(out,"id",&actual_channel) && actual_channel==expected_channel && same(json_string(out,"type"),"channel") &&
        !tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(pin,"chat"),"id",&pin_channel) && pin_channel==expected_channel && !json_decimal(cJSON_GetObjectItemCaseSensitive(journal,"superblock"),"start_id",&start) &&
        !tgfs_bot_integer(pin,"message_id",&mid) && mid==(long long)start && json_semantic_equal(header,cJSON_GetObjectItemCaseSensitive(journal,"superblock"));
    cJSON_Delete(header);cJSON_Delete(out);if(!good) return fail(e,"pinned SUPERBLOCK differs from capture");
    char *text=cJSON_PrintUnformatted(caption),*document=cJSON_PrintUnformatted(body);
    if(!text || !document) {free(text);free(document);return fail(e,"out of memory");}
    if(log_event(fd,"directory_send_pending",operation,e)) {free(text);free(document);return -1;}
#ifndef TGFS_TESTING
    struct timespec pause={1,100000000};
    while(nanosleep(&pause,&pause)) if(errno!=EINTR) {free(text);free(document);return fail(e,"recovery pacing failed");}
#endif
    tgfs_bot_error b;out=NULL;
    int rc=tgfs_bot_send_document(bot,json_string(journal,"storage_chat_id"),"directory.json",document,strlen(document),text,&out,&b);
    if(rc) {
        fail(e,"%s [http=%ld api=%d outcome_unknown=%d retry_after=%u]",b.message,b.http_status,b.api_code,b.outcome_unknown,b.retry_after);
        tgfs_error saved=*e;
        if(upload_rejection(fd,"directory_send_rejected",operation,operation,&b,e)) {free(text);free(document);cJSON_Delete(out);return -1;}
        *e=saved;
    }
    long long channel,returned_channel,returned_size;mid=0;
    const cJSON *chat=cJSON_GetObjectItemCaseSensitive(out,"chat"),*doc=cJSON_GetObjectItemCaseSensitive(out,"document");
    uint64_t boundary;json_decimal(journal,"boundary_id",&boundary);
    if(!rc && (tgfs_bot_integer(out,"message_id",&mid) || mid<=(long long)boundary ||
        tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&channel) || tgfs_bot_integer(chat,"id",&returned_channel) || channel!=returned_channel ||
        !same(json_string(chat,"type"),"channel") || !same(json_string(out,"caption"),text) ||
        !same(json_string(doc,"file_name"),"directory.json") || !json_string(doc,"file_id") ||
        tgfs_bot_integer(doc,"file_size",&returned_size) || returned_size!=(long long)strlen(document))) rc=fail(e,"resumed ADD response mismatch; outcome may be unknown");
    free(text);free(document);cJSON_Delete(out);
    if(rc) return -1;
    char id[32];snprintf(id,sizeof(id),"%lld",mid);
    if(log_event(fd,"directory_publication_confirmed",id,e)) return -1;
    cJSON *value=cJSON_CreateString(id);
    if(!value || !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(report,"directory_ids"),value)) {cJSON_Delete(value);return fail(e,"out of memory");}
    if(!cJSON_AddStringToObject(report,"directory_resume","published")) return fail(e,"out of memory");
    cJSON_ReplaceItemInObjectCaseSensitive(report,"scope",cJSON_CreateString("capture_plus_recovery_response"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"publication",cJSON_CreateString("confirmed"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"resumed",cJSON_CreateBool(1));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"next_step",cJSON_CreateString("capture_after_directory_publication"));
    return 0;
}

static int finish_local_intent(const char *state,const cJSON *intent,const char *commit_id,const char *directory_id,int logfd,tgfs_error *e)
{
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(dir<0) return fail(e,"cannot reopen intent directory");
    cJSON *current=NULL,*finished=NULL;char *raw=NULL;int fd=-1,rc=-1;
    if(fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077)) {fail(e,"intent directory changed permissions");goto done;}
    const char *kind=json_string(intent,"kind");
    const char *primary=same(kind,"put")?"file_commit_id":same(kind,"mkdir")?"child_message_id":NULL;
    current=json_load_at(dir,"intent.json",e);
    if(!current || !cJSON_Compare(current,intent,1)) {fail(e,"intent changed during finalization");goto done;}
    if(same(json_string(intent,"phase"),"complete")) {
        if((primary && !same(json_string(intent,primary),commit_id)) || (directory_id?!same(json_string(intent,"directory_message_id"),directory_id):cJSON_HasObjectItem(intent,"directory_message_id"))) fail(e,"complete intent stored IDs differ from canonical publications");
        else if(fsync(dir)) fail(e,"cannot sync intent directory");else rc=0;
        goto done;
    }
    finished=cJSON_Duplicate(intent,1);if(!finished) {fail(e,"out of memory");goto done;}
    cJSON_DeleteItemFromObjectCaseSensitive(finished,"phase");
    if(primary) cJSON_DeleteItemFromObjectCaseSensitive(finished,primary);
    cJSON_DeleteItemFromObjectCaseSensitive(finished,"directory_message_id");
    if(!cJSON_AddStringToObject(finished,"phase","complete") || (primary && !cJSON_AddStringToObject(finished,primary,commit_id)) ||
        (directory_id && !cJSON_AddStringToObject(finished,"directory_message_id",directory_id))) {fail(e,"out of memory");goto done;}
    raw=cJSON_PrintUnformatted(finished);if(!raw) {fail(e,"out of memory");goto done;}
    fd=openat(dir,"intent.finalize.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) {fail(e,"cannot create intent.finalize.tmp; preserve any existing recovery file");goto done;}
    size_t offset=0,length=strlen(raw);
    while(offset<length) {ssize_t n=write(fd,raw+offset,length-offset);if(n<0 && errno==EINTR) continue;if(n<=0) {fail(e,"cannot write finalized intent");goto done;}offset+=(size_t)n;}
    if(fsync(fd)) {fail(e,"cannot persist finalized intent");goto done;}
    if(close(fd)) {fd=-1;fail(e,"cannot close finalized intent");goto done;}fd=-1;
    if(log_event(logfd,"local_complete_pending",commit_id,e)) goto done;
    if(renameat(dir,"intent.finalize.tmp",dir,"intent.json") || fsync(dir)) {fail(e,"cannot publish finalized intent; local outcome may be unknown");goto done;}
    if(log_event(logfd,"local_complete_confirmed",commit_id,e)) goto done;
    rc=1;
done:
    if(fd>=0) close(fd);
    close(dir);free(raw);cJSON_Delete(current);cJSON_Delete(finished);return rc;
}
int finalize_put(tgfs_bot *bot,const char *state,const cJSON *journal,const cJSON *intent,cJSON *report,int logfd,tgfs_error *e)
{
    const cJSON *commits=cJSON_GetObjectItemCaseSensitive(report,"commit_ids"),*directories=cJSON_GetObjectItemCaseSensitive(report,"directory_ids");
    int replacing=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(intent,"replace"));
    if(!same(json_string(intent,"kind"),"put") || !cJSON_GetArraySize(commits) ||
        (!replacing && !cJSON_GetArraySize(directories)) || !same(json_string(report,"staging"),"verified"))
        return fail(e,"finalize requires verified put, confirmed FILE_COMMIT and ADD for creation");
    uint64_t base=0;
    if(replacing && (!uuid_valid(json_string(intent,"base_version_id")) || json_decimal(intent,"base_commit_id",&base) || !base ||
        cJSON_HasObjectItem(intent,"add_operation_id") || cJSON_HasObjectItem(intent,"directory_document") ||
        cJSON_HasObjectItem(intent,"directory_caption") || cJSON_HasObjectItem(intent,"directory_message_id")))
        return fail(e,"invalid replacement intent fields");
    const char *bad[]={"unobserved_part_indexes","ambiguous_part_indexes","unobserved_known_ids"};
    for(size_t i=0;i<3;i++) if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report,bad[i]))) return fail(e,"finalize blocked by missing or ambiguous parts");
    const char *commit_id=cJSON_GetArrayItem(commits,0)->valuestring,*directory_id=replacing?NULL:cJSON_GetArrayItem(directories,0)->valuestring;
    uint64_t cid,did=0;decimal(commit_id,&cid);if(directory_id) decimal(directory_id,&did);
    if(directory_id && did<=cid) return fail(e,"ADD precedes canonical FILE_COMMIT");
    /* Force live validation of all canonical parts, including those correct in the capture. */
    cJSON *original=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(report,"backlink_repairs"),1),*all=cJSON_CreateArray();
    if(!original || !all) {cJSON_Delete(original);cJSON_Delete(all);return fail(e,"out of memory");}
    const cJSON *parts=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(intent,"commit_document"),"parts"),*part;
    cJSON_ArrayForEach(part,parts) {
        const char *next=part->next?json_string(part->next,"message_id"):commit_id;cJSON *fix=cJSON_CreateObject();
        if(!fix || !cJSON_AddStringToObject(fix,"message_id",json_string(part,"message_id")) ||
            !cJSON_AddStringToObject(fix,"file_commit_id",commit_id) || !cJSON_AddStringToObject(fix,"next_message",next) || !cJSON_AddItemToArray(all,fix)) {
            cJSON_Delete(fix);cJSON_Delete(all);cJSON_Delete(original);return fail(e,"out of memory");
        }
    }
    cJSON_ReplaceItemInObjectCaseSensitive(report,"backlink_repairs",all);
    int checked=repair_backlinks(bot,journal,intent,report,logfd,e);
    cJSON_ReplaceItemInObjectCaseSensitive(report,"backlink_repairs",original);
    if(checked) return -1;
    const cJSON *cr=record_find(journal,commit_id),*dr=directory_id?record_find(journal,directory_id):NULL;
    cJSON *commit=fresh_caption(bot,journal,cr,logfd,e);if(!commit) return -1;
    cJSON *directory=dr?fresh_caption(bot,journal,dr,logfd,e):NULL;if(dr && !directory) {cJSON_Delete(commit);return -1;}
    int same_metadata=json_semantic_equal(commit,cJSON_GetObjectItemCaseSensitive(cr,"record")) &&
        (!dr || json_semantic_equal(directory,cJSON_GetObjectItemCaseSensitive(dr,"record")));cJSON_Delete(directory);
    if(!same_metadata) {cJSON_Delete(commit);return fail(e,"fresh commit or ADD metadata changed");}
    const char *next=json_string(commit,"next_message");
    if(!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(commit,"next_message")) && (replacing || !same(next,directory_id))) {
        cJSON_Delete(commit);return fail(e,replacing?"replacement commit navigation must remain null":"commit navigation conflicts with confirmed ADD");
    }
    cJSON *p=params(json_string(journal,"storage_chat_id")),*out=NULL;
    if(call(bot,"getChat",p,&out,0,e)) {cJSON_Delete(commit);return -1;}
    const cJSON *pin=cJSON_GetObjectItemCaseSensitive(out,"pinned_message");const char *text=json_string(pin,"text");
    cJSON *header=text?json_parse_bytes((const unsigned char *)text,strlen(text),e):NULL;
    uint64_t start;long long pinned,expected_channel,actual_channel,pin_channel;
    int valid=header && !tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&expected_channel) &&
        !tgfs_bot_integer(out,"id",&actual_channel) && actual_channel==expected_channel && same(json_string(out,"type"),"channel") &&
        !tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(pin,"chat"),"id",&pin_channel) && pin_channel==expected_channel && !json_decimal(cJSON_GetObjectItemCaseSensitive(journal,"superblock"),"start_id",&start) &&
        !tgfs_bot_integer(pin,"message_id",&pinned) && pinned==(long long)start && json_semantic_equal(header,cJSON_GetObjectItemCaseSensitive(journal,"superblock"));
    cJSON_Delete(header);cJSON_Delete(out);if(!valid) {cJSON_Delete(commit);return fail(e,"pinned SUPERBLOCK differs from capture");}
    int repaired=directory_id && !same(next,directory_id);
    if(repaired) {
        cJSON_DeleteItemFromObjectCaseSensitive(commit,"next_message");
        if(!cJSON_AddStringToObject(commit,"next_message",directory_id)) {cJSON_Delete(commit);return fail(e,"out of memory");}
        char *raw=cJSON_PrintUnformatted(commit);p=params(json_string(journal,"storage_chat_id"));out=NULL;
        if(!raw || !p || !cJSON_AddNumberToObject(p,"message_id",(double)cid) || !cJSON_AddStringToObject(p,"caption",raw)) {
            free(raw);cJSON_Delete(p);cJSON_Delete(commit);return fail(e,"out of memory");
        }
        if(log_event(logfd,"commit_navigation_pending",commit_id,e)) {free(raw);cJSON_Delete(p);cJSON_Delete(commit);return -1;}
        int rc=call(bot,"editMessageCaption",p,&out,1,e);
        if(!rc) {
            if(message_matches(bot,out,cr,json_string(journal,"storage_chat_id"),commit_id,e)) rc=-1;
            else if(!same(json_string(out,"caption"),raw)) rc=fail(e,"commit edit caption mismatch; outcome may be unknown");
        }
        free(raw);cJSON_Delete(out);if(rc || log_event(logfd,"commit_navigation_confirmed",commit_id,e)) {cJSON_Delete(commit);return -1;}
    }
    cJSON_Delete(commit);
    int updated=finish_local_intent(state,intent,commit_id,directory_id,logfd,e);if(updated<0) return -1;
    if(!cJSON_AddStringToObject(report,"finalize_kind",replacing?"replace":"create") ||
        !cJSON_AddBoolToObject(report,"finalized",1) || !cJSON_AddBoolToObject(report,"intent_updated",updated) ||
        !cJSON_AddBoolToObject(report,"commit_navigation_repaired",repaired)) return fail(e,"out of memory");
    cJSON_ReplaceItemInObjectCaseSensitive(report,"scope",cJSON_CreateString("capture_plus_live_verification"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"local_phase",cJSON_CreateString("complete"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"next_step",cJSON_CreateString("intent_complete"));
    return 0;
}

int finalize_metadata(tgfs_bot *bot,const char *state,const cJSON *journal,const cJSON *intent,cJSON *report,int logfd,tgfs_error *e)
{
    const char *kind=json_string(intent,"kind");int mkdir=same(kind,"mkdir");
    const cJSON *dirs=cJSON_GetObjectItemCaseSensitive(report,"directory_ids"),*children=cJSON_GetObjectItemCaseSensitive(report,"child_ids");
    if((!mkdir && !same(kind,"unlink")) || !cJSON_GetArraySize(dirs) || (mkdir && !cJSON_GetArraySize(children)))
        return fail(e,"metadata finalize requires confirmed publications");
    const cJSON *saved=cJSON_GetObjectItemCaseSensitive(intent,"directory_caption");
    const cJSON *body=cJSON_GetObjectItemCaseSensitive(intent,"directory_document"),*ops=cJSON_GetObjectItemCaseSensitive(body,"operations");
    const cJSON *op=cJSON_GetArrayItem(ops,0);size_t format;
    if(directory_operations_valid(body,e)) return -1;
    if(cJSON_GetArraySize(ops)!=1 || !same(json_string(op,"action"),mkdir?"ADD":"REMOVE") ||
        !same(json_string(op,"operation_id"),json_string(intent,mkdir?"add_operation_id":"remove_operation_id")) ||
        !same(json_string(saved,"object_id"),json_string(intent,"parent_id")) ||
        !same(json_string(saved,"type"),"DIRECTORY") || !same(json_string(saved,"magic"),"TGFS") ||
        json_count(saved,"format_version",&format) || format!=1 ||
        (mkdir?!same(json_string(op,"object_id"),json_string(intent,"object_id")):
        !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(op,"observed_adds"),cJSON_GetObjectItemCaseSensitive(intent,"observed_adds"),1)))
        return fail(e,"metadata intent fields disagree with saved operation");
    const char *saved_path=json_string(intent,"path");char path[4097];
    if(!saved_path || saved_path[0]!='/' || strlen(saved_path)>4096) return fail(e,"invalid metadata intent path");
    memcpy(path,saved_path,strlen(saved_path)+1);size_t length=strlen(path);
    while(mkdir && length>1 && path[length-1]=='/') path[--length]=0;
    if(!same(strrchr(path,'/')+1,json_string(op,"name"))) return fail(e,"metadata intent path disagrees with operation name");
    if(mkdir) {
        const cJSON *cc=cJSON_GetObjectItemCaseSensitive(intent,"child_caption");
        const cJSON *cb=cJSON_GetObjectItemCaseSensitive(intent,"child_document");
        if(!same(json_string(cc,"object_id"),json_string(intent,"object_id")) ||
            !same(json_string(cc,"parent_id"),json_string(intent,"parent_id")) ||
            !same(json_string(cc,"magic"),"TGFS") || !same(json_string(cc,"type"),"DIRECTORY") ||
            json_count(cc,"format_version",&format) || format!=1 || !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cc,"next_message")) ||
            directory_operations_valid(cb,e) || cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cb,"operations")))
            return fail(e,"saved child DIRECTORY disagrees with mkdir intent");
    }
    const char *directory_id=cJSON_GetArrayItem(dirs,0)->valuestring;
    const char *child_id=mkdir?cJSON_GetArrayItem(children,0)->valuestring:NULL;
    uint64_t did,cid=0;decimal(directory_id,&did);if(child_id) decimal(child_id,&cid);
    if(mkdir && cid>=did) return fail(e,"mkdir ADD precedes child DIRECTORY");
    const cJSON *dr=record_find(journal,directory_id),*cr=mkdir?record_find(journal,child_id):NULL;
    cJSON *directory=fresh_caption(bot,journal,dr,logfd,e);if(!directory) return -1;
    int good=json_semantic_equal(directory,cJSON_GetObjectItemCaseSensitive(dr,"record")) &&
        cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(directory,"next_message"));cJSON_Delete(directory);
    if(!good) return fail(e,"fresh operation DIRECTORY metadata or navigation differs");
    cJSON *child=mkdir?fresh_caption(bot,journal,cr,logfd,e):NULL;if(mkdir && !child) return -1;
    if(child && (!json_semantic_equal(child,cJSON_GetObjectItemCaseSensitive(cr,"record")) ||
        (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(child,"next_message")) && !same(json_string(child,"next_message"),directory_id)))) {
        cJSON_Delete(child);return fail(e,"fresh child DIRECTORY metadata or navigation conflicts");
    }
    cJSON *p=params(json_string(journal,"storage_chat_id")),*out=NULL;
    if(call(bot,"getChat",p,&out,0,e)) {cJSON_Delete(child);return -1;}
    const cJSON *pin=cJSON_GetObjectItemCaseSensitive(out,"pinned_message");const char *text=json_string(pin,"text");
    cJSON *header=text?json_parse_bytes((const unsigned char *)text,strlen(text),e):NULL;
    uint64_t start;long long mid,channel,actual,pin_channel;
    good=header && !tgfs_bot_channel_id(json_string(journal,"storage_chat_id"),&channel) &&
        !tgfs_bot_integer(out,"id",&actual) && actual==channel && same(json_string(out,"type"),"channel") &&
        !tgfs_bot_integer(cJSON_GetObjectItemCaseSensitive(pin,"chat"),"id",&pin_channel) && pin_channel==channel &&
        !json_decimal(cJSON_GetObjectItemCaseSensitive(journal,"superblock"),"start_id",&start) &&
        !tgfs_bot_integer(pin,"message_id",&mid) && mid==(long long)start && json_semantic_equal(header,cJSON_GetObjectItemCaseSensitive(journal,"superblock"));
    cJSON_Delete(header);cJSON_Delete(out);if(!good) {cJSON_Delete(child);return fail(e,"pinned SUPERBLOCK differs from capture");}
    int repaired=mkdir && !same(json_string(child,"next_message"),directory_id);
    if(repaired) {
        cJSON_DeleteItemFromObjectCaseSensitive(child,"next_message");
        if(!cJSON_AddStringToObject(child,"next_message",directory_id)) {cJSON_Delete(child);return fail(e,"out of memory");}
        char *raw=cJSON_PrintUnformatted(child);p=params(json_string(journal,"storage_chat_id"));out=NULL;
        if(!raw || !p || !cJSON_AddNumberToObject(p,"message_id",(double)cid) || !cJSON_AddStringToObject(p,"caption",raw)) {free(raw);cJSON_Delete(p);cJSON_Delete(child);return fail(e,"out of memory");}
        if(log_event(logfd,"child_navigation_pending",child_id,e)) {free(raw);cJSON_Delete(p);cJSON_Delete(child);return -1;}
        int rc=call(bot,"editMessageCaption",p,&out,1,e);
        if(!rc && message_matches(bot,out,cr,json_string(journal,"storage_chat_id"),child_id,e)) rc=-1;
        if(!rc && !same(json_string(out,"caption"),raw)) rc=fail(e,"child edit response mismatch; outcome may be unknown");
        free(raw);cJSON_Delete(out);if(rc || log_event(logfd,"child_navigation_confirmed",child_id,e)) {cJSON_Delete(child);return -1;}
    }
    cJSON_Delete(child);
    int updated=finish_local_intent(state,intent,child_id?child_id:directory_id,directory_id,logfd,e);if(updated<0) return -1;
    if(!cJSON_AddStringToObject(report,"finalize_kind",kind) || !cJSON_AddBoolToObject(report,"finalized",1) ||
        !cJSON_AddBoolToObject(report,"intent_updated",updated) || !cJSON_AddBoolToObject(report,"child_navigation_repaired",repaired)) return fail(e,"out of memory");
    cJSON_ReplaceItemInObjectCaseSensitive(report,"scope",cJSON_CreateString("capture_plus_live_verification"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"local_phase",cJSON_CreateString("complete"));
    cJSON_ReplaceItemInObjectCaseSensitive(report,"next_step",cJSON_CreateString("intent_complete"));
    return 0;
}

/* Share the same durable, locked recovery journal with the upload writer. */
int upload_log_event(int fd,const char *event,const char *id,tgfs_error *e)
{
    return log_event(fd,event,id,e);
}
int upload_request_event(int fd,const char *event,const char *id,const char *operation,tgfs_error *e)
{
    char line[200];int n=snprintf(line,sizeof(line),"{\"event\":\"%s\",\"message_id\":\"%s\",\"operation_id\":\"%s\"}\n",event,id,operation);
    if(n<0 || (size_t)n>=sizeof(line)) return fail(e,"upload log overflow");
    return append_log(fd,line,(size_t)n,e);
}
/* 0: no logged request; 1: unresolved; 2: rejected; 3: confirmed commit. */
int upload_attempts(int fd,const char *operation,unsigned char attempted[4096],int *commit_attempted,tgfs_error *e)
{
    struct stat st;
    if(fstat(fd,&st) || st.st_size>TGFS_MAX_JSON || lseek(fd,0,SEEK_SET)<0) return fail(e,"cannot inspect upload recovery log");
    FILE *stream=fdopen(dup(fd),"r");if(!stream) return fail(e,"cannot read upload recovery log");
    unsigned char owned[4096]={0};int commit_owned=0;
    uint64_t deadlines[4096]={0},commit_deadline=0;
    char *line=NULL;size_t capacity=0,total=0;ssize_t n;int rc=0;
    while((n=getline(&line,&capacity,stream))>=0) {
        total+=(size_t)n;
        if(total>TGFS_MAX_JSON) {rc=fail(e,"upload recovery log too large");break;}
        cJSON *entry=json_parse_bytes((const unsigned char *)line,(size_t)n,e);
        if(!entry || !json_string(entry,"event")) {cJSON_Delete(entry);rc=fail(e,"invalid upload recovery log");break;}
        const char *event=json_string(entry,"event");
        int chunk_pending=same(event,"upload_chunk_pending"),chunk_rejected=same(event,"upload_chunk_rejected");
        int commit_pending=same(event,"upload_commit_pending"),commit_rejected=same(event,"upload_commit_rejected");
        if(chunk_pending || chunk_rejected || commit_pending || commit_rejected) {
            uint64_t index=0;
            if((chunk_pending || chunk_rejected) && (decimal(json_string(entry,"message_id"),&index) || index>=4096))
                rc=fail(e,"invalid upload attempt index");
            else {
                int commit=commit_pending || commit_rejected;
                int previous=commit?*commit_attempted:attempted[index];
                int ours=same(json_string(entry,"operation_id"),operation);
                if(commit && !same(json_string(entry,"message_id"),operation)) rc=fail(e,"commit operation differs in recovery log");
                else if(chunk_pending || commit_pending) {
                    if(previous==1 || previous==3 || (cJSON_HasObjectItem(entry,"operation_id") && !ours)) rc=fail(e,"conflicting upload attempts in recovery log");
                    else if(commit) {*commit_attempted=1;commit_owned=ours;commit_deadline=0;}
                    else {attempted[index]=1;owned[index]=(unsigned char)ours;deadlines[index]=0;}
                } else {
                    size_t http,api;uint64_t when;
                    if(previous!=1 || !ours || !(commit?commit_owned:owned[index]) ||
                        json_count(entry,"http_status",&http) || json_count(entry,"api_code",&api) ||
                        http<400 || http>=500 || api!=http || json_decimal(entry,"retry_not_before",&when) || (api==429 && !when))
                        rc=fail(e,"invalid upload rejection evidence");
                    else if(commit) {*commit_attempted=2;commit_deadline=when;}
                    else {attempted[index]=2;deadlines[index]=when;}
                }
            }
        }
        if(same(event,"upload_commit_confirmed")) *commit_attempted=3;
        cJSON_Delete(entry);if(rc) break;
    }
    if(ferror(stream)) rc=fail(e,"cannot read upload recovery log");
    free(line);fclose(stream);
    uint64_t deadline=commit_deadline;
    for(size_t i=0;i<4096;i++) if(deadlines[i]>deadline) deadline=deadlines[i];
    if(!rc && deadline) {
        time_t now=time(NULL);
        if(now<0 || (uint64_t)now<=deadline) rc=fail(e,"upload retry cooldown active; retry after Unix time %llu",(unsigned long long)deadline);
    }
    return rc;
}
int verify_upload_part(tgfs_bot *bot,const cJSON *journal,const cJSON *record,int fd,tgfs_error *e)
{
    cJSON *fresh=fresh_caption(bot,journal,record,fd,e);if(!fresh) return -1;
    int good=cJSON_Compare(fresh,cJSON_GetObjectItemCaseSensitive(record,"record"),1) &&
        cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fresh,"file_commit_id")) &&
        cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fresh,"next_message"));
    cJSON_Delete(fresh);
    return good?0:fail(e,"fresh uncommitted CHUNK metadata differs or has navigation");
}
