#include "tgfs/scan.h"
#include "tgfs/bot_api.h"
#include "internal.h"
#include <fcntl.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

static int same(const char *a, const char *b) { return a && b && !strcmp(a,b); }
static cJSON *params(const char *chat)
{
    cJSON *p=cJSON_CreateObject();
    if (!p || !cJSON_AddStringToObject(p,"chat_id",chat)) { cJSON_Delete(p); return NULL; }
    return p;
}
static int request(tgfs_bot *bot,const char *method,cJSON *p,cJSON **out,tgfs_bot_error *e)
{
    cJSON_Delete(*out); *out=NULL;
    int rc=tgfs_bot_call(bot,method,p,out,e); cJSON_Delete(p); return rc;
}
/* Replacing an intent is atomic; no next network mutation starts until fsync. */
static int persist(int dir,cJSON *intent,const char *phase,long long id)
{
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"phase");
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"superblock_message_id");
    char text[32]; snprintf(text,sizeof(text),"%lld",id);
    if (!cJSON_AddStringToObject(intent,"phase",phase) ||
        !cJSON_AddStringToObject(intent,"superblock_message_id",text)) return -1;
    char *raw=cJSON_PrintUnformatted(intent);
    if (!raw) return -1;
    int rc=stage_intent_at(dir,"intent.tmp",raw,NULL);
    free(raw);
    if (!rc && (renameat(dir,"intent.tmp",dir,"intent.json") || fsync(dir))) rc=-1;
    return rc;
}
static int channel_is(const cJSON *channel,long long expected)
{
    long long id;
    return !tgfs_bot_integer(channel,"id",&id) && id==expected && same(json_string(channel,"type"),"channel");
}

static int message_is(const cJSON *m,long long chat,long long id,const char *text)
{
    long long mid;
    return !tgfs_bot_integer(m,"message_id",&mid) && mid>0 && (!id || mid==id) &&
        channel_is(cJSON_GetObjectItemCaseSensitive(m,"chat"),chat) && same(json_string(m,"text"),text);
}
static void header_text(char text[320],long long id,const char *uuid)
{
    snprintf(text,320,"{\"magic\":\"TGFS\",\"format_version\":1,\"type\":\"SUPERBLOCK\",\"start_id\":\"%lld\",\"root_object_id\":\"%s\",\"next_message\":null}",id,uuid);
}

static int lock_state(const char *state)
{
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) return -1;
    struct stat st;
    if(fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077) || flock(dir,LOCK_EX|LOCK_NB)) {
        close(dir);
        return -1;
    }
    return dir;
}

static cJSON *load_intent(int dir)
{
    struct stat st;
    if(fstatat(dir,"intent.json",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_nlink!=1 ||
        st.st_uid!=geteuid() || (st.st_mode&0077)) return NULL;
    tgfs_error e={0};
    return json_load_at(dir,"intent.json",&e);
}

static int save_rejection(int dir,cJSON *intent,long long id,const tgfs_bot_error *api)
{
    time_t now=time(NULL);
    if(now<0) return -1;
    char deadline[32];
    snprintf(deadline,sizeof(deadline),"%llu",api->retry_after?(unsigned long long)now+api->retry_after:0);
    if(!cJSON_AddNumberToObject(intent,id?"pin_http_status":"send_http_status",api->http_status) ||
        !cJSON_AddStringToObject(intent,id?"pin_retry_not_before":"send_retry_not_before",deadline)) return -1;
    return persist(dir,intent,id?"pin_rejected":"send_rejected",id);
}

static const char *resume_send_error(const cJSON *intent,const char *chat,long long bot_id)
{
    uint64_t mid,saved_bot,deadline;
    size_t status;
    if(!same(json_string(intent,"phase"),"send_rejected") ||
        !same(json_string(intent,"storage_chat_id"),chat) || !uuid_valid(json_string(intent,"root_object_id")) ||
        json_decimal(intent,"initializer_bot_id",&saved_bot) || saved_bot!=(uint64_t)bot_id ||
        json_decimal(intent,"superblock_message_id",&mid) || mid ||
        json_count(intent,"send_http_status",&status) || status<400 || status>=500 ||
        json_decimal(intent,"send_retry_not_before",&deadline) || (status==429 && !deadline))
        return "resume-send requires a confirmed sendMessage rejection for this bot and channel";
    if(deadline) {
        time_t now=time(NULL);
        if(now<0 || (uint64_t)now<=deadline) return "sendMessage retry cooldown active";
    }
    return NULL;
}

/* Read the known message through a silent service copy, then delete that copy. */
static int inspect_header(tgfs_bot *bot,const char *storage,long long storage_id,const char *service,
    long long service_id,long long id,const char *placeholder,const char *header,tgfs_bot_error *api,const char **error)
{
    cJSON *out=NULL;
    int state=-1;
    if(request(bot,"getChat",params(service),&out,api)) goto done;
    if(!channel_is(out,service_id)) {*error="service channel identity mismatch";goto done;}
    cJSON *p=params(service);
    if(!p || !cJSON_AddStringToObject(p,"from_chat_id",storage) ||
        !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddBoolToObject(p,"disable_notification",1)) {
        *error="cannot allocate header inspection request";cJSON_Delete(p);goto done;
    }
    if(request(bot,"forwardMessage",p,&out,api)) {
        if(api->outcome_unknown) fprintf(stderr,"init: a service copy may remain in chat %s\n",service);
        goto done;
    }
    long long copy,original;
    if(!channel_is(cJSON_GetObjectItemCaseSensitive(out,"chat"),service_id) ||
        tgfs_bot_integer(out,"message_id",&copy) || copy<=0) {
        *error="invalid service copy response; inspect the service channel";goto done;
    }
    const cJSON *origin=cJSON_GetObjectItemCaseSensitive(out,"forward_origin");
    int valid=same(json_string(origin,"type"),"channel") &&
        channel_is(cJSON_GetObjectItemCaseSensitive(origin,"chat"),storage_id) &&
        !tgfs_bot_integer(origin,"message_id",&original) && original==id;
    const char *text=json_string(out,"text");
    int observed=valid && same(text,header)?1:valid && same(text,placeholder)?0:-1;
    p=params(service);
    if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)copy)) {
        cJSON_Delete(p);
        *error="cannot allocate service cleanup request";
        fprintf(stderr,"init: cleanup pending service message_id=%lld\n",copy);
        goto done;
    }
    if(request(bot,"deleteMessage",p,&out,api) || !cJSON_IsTrue(out)) {
        *error="service copy cleanup failed";
        fprintf(stderr,"init: cleanup pending service message_id=%lld\n",copy);
        goto done;
    }
    if(observed<0) *error="saved message origin or text differs from initialization intent";
    state=observed;
done:
    cJSON_Delete(out);
    return state;
}

static int finalize_existing(const char *state,const char *chat,long long chat_id,tgfs_bot *bot,tgfs_bot_error *api,const char **error)
{
    int dir=lock_state(state),rc=-1;
    cJSON *intent=NULL,*out=NULL;
    *error="cannot lock private initializer state";
    if(dir<0) goto done;
    *error="invalid initializer intent";
    intent=load_intent(dir);
    if(!intent) goto done;
    const char *phase=json_string(intent,"phase"),*root=json_string(intent,"root_object_id");
    uint64_t mid;
    if(!same(json_string(intent,"storage_chat_id"),chat) || !uuid_valid(root) ||
        (!same(phase,"pin_pending") && !same(phase,"complete")) || json_decimal(intent,"superblock_message_id",&mid) ||
        !mid || mid>LLONG_MAX) goto done;
    char text[320];header_text(text,(long long)mid,root);
    *error=NULL;
    if(request(bot,"getChat",params(chat),&out,api)) goto done;
    *error="pinned SUPERBLOCK differs from saved initialization";
    if(!channel_is(out,chat_id) ||
        !message_is(cJSON_GetObjectItemCaseSensitive(out,"pinned_message"),chat_id,(long long)mid,text)) goto done;
    *error="cannot persist completed initialization";
    if(!same(phase,"complete") && persist(dir,intent,"complete",(long long)mid)) goto done;
    if(fsync(dir)) goto done;
    printf("OK init verified storage=%lld start_id=%llu root_object_id=%s\n",chat_id,(unsigned long long)mid,root);rc=0;
done:
    cJSON_Delete(out);cJSON_Delete(intent);if(dir>=0) close(dir);return rc;
}

void tgfs_init_help(FILE *stream)
{
    fputs("usage: tgfs init (--new-volume | --resume-header | --finalize-existing) --token-file PATH --storage-chat-id ID --state-dir DIRECTORY [options]\n"
          "Create and pin a SUPERBLOCK in a channel explicitly confirmed to be uninitialized.\n"
          "  --new-volume             Assert no existing TGFS volume, including unpinned headers.\n"
          "  --resume-send            With --new-volume, retry an explicitly rejected first send.\n"
          "  --resume-header          Inspect the saved message and continue editing or pinning.\n"
          "  --service-chat-id ID     Separate private service channel for --resume-header.\n"
          "  --finalize-existing      Verify matching pinned header and complete existing local intent.\n"
          "  --state-dir DIRECTORY    New directory for creation, existing private directory for recovery.\n"
          "  --token-file PATH        Owner-only bot token file.\n"
          "  --storage-chat-id ID     Full negative Bot API channel ID.\n"
          "  --proxy URL | --direct   Same routing options as probe.\n"
          "  --timeout-seconds N      1..300 seconds, default 60.\n"
          "  -h, --help               Show help without network.\n"
          "Run only one initializer for the channel.\n"
          "On failure preserve the state directory. --resume-send requires a confirmed rejection.\n",stream);
}
int tgfs_init_main(int argc,char **argv)
{
    const char *token=NULL,*chat=NULL,*state=NULL,*service=NULL,*proxy=NULL,*timeout_text=NULL;
    int confirmed=0,finalize=0,resume_send=0,resume_header=0;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_init_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--resume-header")) {if(resume_header) goto usage;resume_header=1;continue;}
        if(same(argv[i],"--resume-send")) {if(resume_send) goto usage;resume_send=1;continue;}
        if(same(argv[i],"--finalize-existing")) {if(finalize) goto usage;finalize=1;continue;}
        if(same(argv[i],"--new-volume")) {if(confirmed) goto usage; confirmed=1;continue;}
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--storage-chat-id")) slot=&chat;
        else if(same(argv[i],"--service-chat-id")) slot=&service;
        else if(same(argv[i],"--state-dir")) slot=&state;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    long long chat_id,service_id=0,bot_id=0,id=0; uint64_t timeout=60;
    if(confirmed+finalize+resume_header!=1 || (resume_send && !confirmed) || !token || !state || !*state || tgfs_bot_channel_id(chat,&chat_id) ||
        (resume_header ? (tgfs_bot_channel_id(service,&service_id) || service_id==chat_id) : service!=NULL) ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300))) goto usage;
    tgfs_bot *bot=NULL; tgfs_bot_error e={0};
    cJSON *out=NULL,*p=NULL,*intent=NULL;
    char uuid[37],placeholder[128],header[320];
    int pin_unknown=0,header_observed=0;
    const char *stage="client",*local=NULL;
    int dir=-1,rc=1;
    if(tgfs_bot_open(token,(unsigned)timeout,&bot,&e) || tgfs_bot_set_proxy(bot,proxy,&e)) goto done;
    if(finalize) {stage="finalize existing";rc=finalize_existing(state,chat,chat_id,bot,&e,&local)?1:0;goto done;}
    stage="getMe";
    if(request(bot,"getMe",cJSON_CreateObject(),&out,&e)) goto done;
    if(tgfs_bot_integer(out,"id",&bot_id) || bot_id<=0 ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"is_bot"))) {local="invalid bot identity";goto done;}
    stage="load recovery intent";
    if(resume_send || resume_header) {
        dir=lock_state(state);
        if(dir<0) {local="cannot lock private initializer state";goto done;}
        intent=load_intent(dir);
        if(resume_send) {
            local=resume_send_error(intent,chat,bot_id);
            if(local) goto done;
            cJSON_DeleteItemFromObjectCaseSensitive(intent,"send_http_status");
            cJSON_DeleteItemFromObjectCaseSensitive(intent,"send_retry_not_before");
        } else {
            const char *phase=json_string(intent,"phase");
            uint64_t saved_id,saved_bot;
            if(!same(json_string(intent,"storage_chat_id"),chat) || !uuid_valid(json_string(intent,"root_object_id")) ||
                json_decimal(intent,"superblock_message_id",&saved_id) || !saved_id || saved_id>TGFS_BOT_MAX_EXACT_ID ||
                (cJSON_HasObjectItem(intent,"initializer_bot_id") &&
                 (json_decimal(intent,"initializer_bot_id",&saved_bot) || saved_bot!=(uint64_t)bot_id)) ||
                (!same(phase,"edit_pending") && !same(phase,"pin_ready") && !same(phase,"pin_pending") && !same(phase,"pin_rejected"))) {
                local="resume-header requires a saved edit or pin intent for this bot and channel";goto done;
            }
            id=(long long)saved_id;
            pin_unknown=same(phase,"pin_pending");
        }
        snprintf(uuid,sizeof(uuid),"%s",json_string(intent,"root_object_id"));
    }
    stage="getChat";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    long long returned;
    if(!channel_is(out,chat_id)) {
        local="requested channel identity not confirmed";goto done;
    }
    if(resume_header) header_text(header,id,uuid);
    if(cJSON_HasObjectItem(out,"pinned_message")) {
        if(resume_header && message_is(cJSON_GetObjectItemCaseSensitive(out,"pinned_message"),chat_id,id,header))
            goto complete;
        local="channel already has a pin; initialization refused";goto done;
    }
    if(pin_unknown) {local="previous pin outcome is unknown and no matching pin is visible";goto done;}
    if(resume_header && same(json_string(intent,"phase"),"pin_rejected")) {
        size_t status;uint64_t deadline;
        if(json_count(intent,"pin_http_status",&status) || status<400 || status>=500 ||
            json_decimal(intent,"pin_retry_not_before",&deadline) || (status==429 && !deadline)) {
            local="invalid pin rejection evidence";goto done;
        }
        time_t now=time(NULL);
        if(deadline && (now<0 || (uint64_t)now<=deadline)) {local="pin retry cooldown active";goto done;}
    }
    stage="getChatMember"; p=params(chat);
    if(!p || !cJSON_AddNumberToObject(p,"user_id",(double)bot_id)) {local="out of memory";goto done;}
    { cJSON *q=p;p=NULL; if(request(bot,"getChatMember",q,&out,&e)) goto done; }
    const cJSON *user=cJSON_GetObjectItemCaseSensitive(out,"user");
    if(tgfs_bot_integer(user,"id",&returned) || returned!=bot_id ||
        !same(json_string(out,"status"),"administrator") ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"can_post_messages")) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"can_edit_messages"))) {
        local="bot needs channel administrator post/edit rights";goto done;
    }
    if(resume_header) {
        stage="inspect saved header";
        snprintf(placeholder,sizeof(placeholder),"TGFS_INIT_PENDING %s",uuid);
        header_observed=inspect_header(bot,chat,chat_id,service,service_id,id,placeholder,header,&e,&local);
        if(header_observed<0) goto done;
        if(!header_observed && !same(json_string(intent,"phase"),"edit_pending")) {
            local="saved pin intent requires the complete header";goto done;
        }
        goto edit_or_pin;
    }
    stage="intent";
    if(!resume_send) {
        if(mkdir(state,0700)) {local="state directory must not already exist; preserve existing intents";goto done;}
        char *parent=strdup(state);
        if (!parent) {local="out of memory";goto done;}
        char *slash=strrchr(parent,'/');
        if(slash) { if(slash==parent) slash[1]=0; else *slash=0; }
        else { free(parent); parent=strdup("."); }
        int parentfd=parent?open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;
        free(parent);
        if(parentfd<0) {local="cannot open state parent directory";goto done;}
        int synced=fsync(parentfd);close(parentfd);
        if(synced) {local="cannot persist state directory entry";goto done;}
        dir=lock_state(state);
        if(dir<0) {local="cannot lock state directory";goto done;}
        unsigned char bytes[16];
        if(RAND_bytes(bytes,16)!=1) {local="UUID randomness unavailable";goto done;}
        bytes[6]=(unsigned char)((bytes[6]&15)|64); bytes[8]=(unsigned char)((bytes[8]&63)|128);
        snprintf(uuid,sizeof(uuid),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
            bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
        char bot_text[32];snprintf(bot_text,sizeof(bot_text),"%lld",bot_id);
        intent=cJSON_CreateObject();
        if(!intent || !cJSON_AddStringToObject(intent,"storage_chat_id",chat) ||
            !cJSON_AddStringToObject(intent,"initializer_bot_id",bot_text) || !cJSON_AddStringToObject(intent,"root_object_id",uuid)) {
            local="cannot persist initialization intent";goto done;
        }
    }
    if(persist(dir,intent,"send_pending",0)) {local="cannot persist send barrier";goto done;}
    stage="sendMessage"; p=params(chat);
    snprintf(placeholder,sizeof(placeholder),"TGFS_INIT_PENDING %s",uuid);
    if(!p || !cJSON_AddStringToObject(p,"text",placeholder) || !cJSON_AddBoolToObject(p,"disable_notification",1)) {
        local="out of memory";goto done;
    }
    cJSON *send=p;p=NULL;
    if(request(bot,"sendMessage",send,&out,&e)) {
        if(upload_rejected(&e) && save_rejection(dir,intent,0,&e))
            local="cannot persist confirmed sendMessage rejection";
        goto done;
    }
    if(!message_is(out,chat_id,0,placeholder) || tgfs_bot_integer(out,"message_id",&id)) {
        local="unexpected placeholder response; inspect channel before recovery";goto done;
    }
    if(persist(dir,intent,"edit_pending",id)) {local="cannot persist source message ID";goto done;}
    header_text(header,id,uuid);
edit_or_pin:
    if(!header_observed) {
        if(resume_header) {
            stage="getChat before edit";
            if(request(bot,"getChat",params(chat),&out,&e)) goto done;
            if(!channel_is(out,chat_id) || cJSON_HasObjectItem(out,"pinned_message")) {
                local="channel pin changed; initialization stopped";goto done;
            }
        }
        stage="editMessageText";p=params(chat);
        if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddStringToObject(p,"text",header)) {
            local="out of memory";goto done;
        }
        cJSON *edit=p;p=NULL;
        if(request(bot,"editMessageText",edit,&out,&e)) goto done;
        if(!message_is(out,chat_id,id,header)) {local="edited SUPERBLOCK response mismatch";goto done;}
    }
    if(same(json_string(intent,"phase"),"edit_pending") && persist(dir,intent,"pin_ready",id)) {
        local="cannot persist edited header";goto done;
    }
    /* Recheck pin immediately before pinning. This is not a distributed lock. */
    stage="getChat before pin";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    if(!channel_is(out,chat_id) || cJSON_HasObjectItem(out,"pinned_message")) {
        local="channel pin changed; initialization stopped";goto done;
    }
    stage="pinChatMessage"; p=params(chat);
    if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddBoolToObject(p,"disable_notification",1)) {
        local="out of memory";goto done;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"pin_http_status");
    cJSON_DeleteItemFromObjectCaseSensitive(intent,"pin_retry_not_before");
    if(persist(dir,intent,"pin_pending",id)) {local="cannot persist pin intent";goto done;}
    cJSON *pin=p;p=NULL;
    if(request(bot,"pinChatMessage",pin,&out,&e)) {
        if(upload_rejected(&e) && save_rejection(dir,intent,id,&e)) local="cannot persist confirmed pin rejection";
        goto done;
    }
    if(!cJSON_IsTrue(out)) {local="pin was not confirmed";goto done;}
    stage="verify pin";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    if(!channel_is(out,chat_id) ||
        !message_is(cJSON_GetObjectItemCaseSensitive(out,"pinned_message"),chat_id,id,header)) {
        local="pinned SUPERBLOCK verification failed";goto done;
    }
complete:
    stage="persist complete";
    if(persist(dir,intent,"complete",id)) {local="cannot persist completed initialization";goto done;}
    printf("OK init storage=%lld start_id=%lld root_object_id=%s\n",chat_id,id,uuid);rc=0;
done:
    if(rc) fprintf(stderr,"FAIL init %s: %s [message_id=%lld http=%ld api=%d outcome_unknown=%d retry_after=%u]\n",
        stage,local?local:e.message,id,e.http_status,e.api_code,e.outcome_unknown,e.retry_after);
    if(dir>=0) close(dir);
    cJSON_Delete(p);cJSON_Delete(out);cJSON_Delete(intent);tgfs_bot_close(bot);
    return rc;
usage:
    tgfs_init_help(stderr);return 2;
}
