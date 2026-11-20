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
    int fd=openat(dir,"intent.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) { free(raw); return -1; }
    size_t size=strlen(raw),done=0; int rc=0;
    while(done<size) { ssize_t n=write(fd,raw+done,size-done); if(n<=0) {rc=-1;break;} done+=(size_t)n; }
    if(fsync(fd)) rc=-1;
    if(close(fd)) rc=-1;
    free(raw);
    if (!rc && (renameat(dir,"intent.tmp",dir,"intent.json") || fsync(dir))) rc=-1;
    return rc;
}
static int message_is(const cJSON *m,long long chat,long long id,const char *text)
{
    long long mid,cid;
    const cJSON *c=cJSON_GetObjectItemCaseSensitive(m,"chat");
    return !tgfs_bot_integer(m,"message_id",&mid) && mid>0 && (!id || mid==id) &&
        !tgfs_bot_integer(c,"id",&cid) && cid==chat && same(json_string(c,"type"),"channel") &&
        same(json_string(m,"text"),text);
}
static void header_text(char text[320],long long id,const char *uuid)
{
    snprintf(text,320,"{\"magic\":\"TGFS\",\"format_version\":1,\"type\":\"SUPERBLOCK\",\"start_id\":\"%lld\",\"root_object_id\":\"%s\",\"next_message\":null}",id,uuid);
}

static int finalize_existing(const char *state,const char *chat,long long chat_id,tgfs_bot *bot,tgfs_bot_error *api,const char **error)
{
    int dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),rc=-1;
    struct stat st;cJSON *intent=NULL,*out=NULL;tgfs_error e={0};
    *error="cannot lock private initializer state";
    if(dir<0 || fstat(dir,&st) || st.st_uid!=geteuid() || (st.st_mode&0077) || flock(dir,LOCK_EX|LOCK_NB)) goto done;
    *error="invalid initializer intent";
    if(fstatat(dir,"intent.json",&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_nlink!=1 ||
        st.st_uid!=geteuid() || (st.st_mode&0077)) goto done;
    intent=json_load_at(dir,"intent.json",&e);
    if(!intent) goto done;
    const char *phase=json_string(intent,"phase"),*root=json_string(intent,"root_object_id");
    uint64_t mid;
    if(!same(json_string(intent,"storage_chat_id"),chat) || !uuid_valid(root) ||
        (!same(phase,"pin_pending") && !same(phase,"complete")) || json_decimal(intent,"superblock_message_id",&mid) ||
        !mid || mid>LLONG_MAX) goto done;
    char text[320];header_text(text,(long long)mid,root);
    *error=NULL;
    if(request(bot,"getChat",params(chat),&out,api)) goto done;
    long long returned;
    *error="pinned SUPERBLOCK differs from saved initialization";
    if(tgfs_bot_integer(out,"id",&returned) || returned!=chat_id || !same(json_string(out,"type"),"channel") ||
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
    fputs("usage: tgfs init (--new-volume | --finalize-existing) --token-file PATH --storage-chat-id ID --state-dir DIRECTORY [options]\n"
          "Create and pin a SUPERBLOCK in a channel explicitly confirmed to be uninitialized.\n"
          "  --new-volume             Assert no existing TGFS volume, including unpinned headers.\n"
          "  --finalize-existing      Verify matching pinned header and complete existing local intent.\n"
          "  --state-dir DIRECTORY    New directory for creation, existing private directory for finalization.\n"
          "  --token-file PATH        Owner-only bot token file.\n"
          "  --storage-chat-id ID     Full negative Bot API channel ID.\n"
          "  --proxy URL | --direct   Same routing options as probe.\n"
          "  --timeout-seconds N      1..300 seconds, default 60.\n"
          "  -h, --help               Show help without network.\n"
          "Refuses any existing pin; run only one initializer for the channel.\n"
          "On failure preserve the state directory: automatic retry/resume is not implemented.\n",stream);
}
int tgfs_init_main(int argc,char **argv)
{
    const char *token=NULL,*chat=NULL,*state=NULL,*proxy=NULL,*timeout_text=NULL;
    int confirmed=0,finalize=0;
    if(argc==3 && (same(argv[2],"--help") || same(argv[2],"-h"))) {tgfs_init_help(stdout);return 0;}
    for(int i=2;i<argc;i++) {
        const char **slot=NULL;
        if(same(argv[i],"--finalize-existing")) {if(finalize) goto usage;finalize=1;continue;}
        if(same(argv[i],"--new-volume")) {if(confirmed) goto usage; confirmed=1;continue;}
        if(same(argv[i],"--direct")) {if(proxy) goto usage;proxy="";continue;}
        if(same(argv[i],"--token-file")) slot=&token;
        else if(same(argv[i],"--storage-chat-id")) slot=&chat;
        else if(same(argv[i],"--state-dir")) slot=&state;
        else if(same(argv[i],"--proxy")) slot=&proxy;
        else if(same(argv[i],"--timeout-seconds")) slot=&timeout_text;
        if(!slot || *slot || i+1>=argc) goto usage;
        *slot=argv[++i];
    }
    long long chat_id,bot_id=0,id=0; uint64_t timeout=60;
    if(confirmed+finalize!=1 || !token || !state || !*state || tgfs_bot_channel_id(chat,&chat_id) ||
        (timeout_text && (decimal(timeout_text,&timeout) || !timeout || timeout>300))) goto usage;
    tgfs_bot *bot=NULL; tgfs_bot_error e={0};
    cJSON *out=NULL,*p=NULL,*intent=NULL; char *header=NULL;
    const char *stage="client",*local=NULL;
    int dir=-1,rc=1;
    if(tgfs_bot_open(token,(unsigned)timeout,&bot,&e) || tgfs_bot_set_proxy(bot,proxy,&e)) goto done;
    if(finalize) {stage="finalize existing";rc=finalize_existing(state,chat,chat_id,bot,&e,&local)?1:0;goto done;}
    stage="getMe";
    if(request(bot,"getMe",cJSON_CreateObject(),&out,&e)) goto done;
    if(tgfs_bot_integer(out,"id",&bot_id) || bot_id<=0 ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"is_bot"))) {local="invalid bot identity";goto done;}
    stage="getChat";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    long long returned;
    if(tgfs_bot_integer(out,"id",&returned) || returned!=chat_id || !same(json_string(out,"type"),"channel")) {
        local="requested channel identity not confirmed";goto done;
    }
    if(cJSON_HasObjectItem(out,"pinned_message")) {local="channel already has a pin; initialization refused";goto done;}
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
    stage="intent";
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
    dir=open(state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0 || flock(dir,LOCK_EX|LOCK_NB)) {local="cannot lock state directory";goto done;}
    unsigned char bytes[16]; char uuid[37];
    if(RAND_bytes(bytes,16)!=1) {local="UUID randomness unavailable";goto done;}
    bytes[6]=(unsigned char)((bytes[6]&15)|64); bytes[8]=(unsigned char)((bytes[8]&63)|128);
    snprintf(uuid,sizeof(uuid),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
    intent=cJSON_CreateObject();
    if(!intent || !cJSON_AddStringToObject(intent,"storage_chat_id",chat) ||
        !cJSON_AddStringToObject(intent,"root_object_id",uuid) || persist(dir,intent,"send_pending",0)) {
        local="cannot persist initialization intent";goto done;
    }
    stage="sendMessage"; p=params(chat);
    char placeholder[128]; snprintf(placeholder,sizeof(placeholder),"TGFS_INIT_PENDING %s",uuid);
    if(!p || !cJSON_AddStringToObject(p,"text",placeholder) || !cJSON_AddBoolToObject(p,"disable_notification",1)) {
        local="out of memory";goto done;
    }
    {cJSON *q=p;p=NULL;if(request(bot,"sendMessage",q,&out,&e)) goto done;}
    if(!message_is(out,chat_id,0,placeholder) || tgfs_bot_integer(out,"message_id",&id)) {
        local="unexpected placeholder response; inspect channel before recovery";goto done;
    }
    if(persist(dir,intent,"edit_pending",id)) {local="cannot persist source message ID";goto done;}
    char text[320];header_text(text,id,uuid);
    header=strdup(text);
    if(!header) {local="out of memory";goto done;}
    stage="editMessageText";p=params(chat);
    if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddStringToObject(p,"text",header)) {
        local="out of memory";goto done;
    }
    {cJSON *q=p;p=NULL;if(request(bot,"editMessageText",q,&out,&e)) goto done;}
    if(!message_is(out,chat_id,id,header)) {local="edited SUPERBLOCK response mismatch";goto done;}
    if(persist(dir,intent,"pin_pending",id)) {local="cannot persist pin intent";goto done;}
    /* Recheck pin immediately before pinning. This is not a distributed lock. */
    stage="getChat before pin";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    if(tgfs_bot_integer(out,"id",&returned) || returned!=chat_id || cJSON_HasObjectItem(out,"pinned_message")) {
        local="channel pin changed; initialization stopped";goto done;
    }
    stage="pinChatMessage"; p=params(chat);
    if(!p || !cJSON_AddNumberToObject(p,"message_id",(double)id) || !cJSON_AddBoolToObject(p,"disable_notification",1)) {
        local="out of memory";goto done;
    }
    {cJSON *q=p;p=NULL;if(request(bot,"pinChatMessage",q,&out,&e)) goto done;}
    if(!cJSON_IsTrue(out)) {local="pin was not confirmed";goto done;}
    stage="verify pin";
    if(request(bot,"getChat",params(chat),&out,&e)) goto done;
    if(tgfs_bot_integer(out,"id",&returned) || returned!=chat_id ||
        !message_is(cJSON_GetObjectItemCaseSensitive(out,"pinned_message"),chat_id,id,header)) {
        local="pinned SUPERBLOCK verification failed";goto done;
    }
    if(persist(dir,intent,"complete",id)) {local="cannot persist completed initialization";goto done;}
    printf("OK init storage=%lld start_id=%lld root_object_id=%s\n",chat_id,id,uuid);rc=0;
done:
    if(rc) fprintf(stderr,"FAIL init %s: %s [message_id=%lld http=%ld api=%d outcome_unknown=%d]\n",
        stage,local?local:e.message,id,e.http_status,e.api_code,e.outcome_unknown);
    if(dir>=0) close(dir);
    free(header);cJSON_Delete(p);cJSON_Delete(out);cJSON_Delete(intent);tgfs_bot_close(bot);
    return rc;
usage:
    tgfs_init_help(stderr);return 2;
}
