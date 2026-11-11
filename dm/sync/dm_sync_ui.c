// 云存档游戏内页面模型，基于同步核心的只读状态和带版本核验的命令。
// In-game cloud-save page model based on read-only core status and version-checked commands.
#include "dm_sync.h"
#include "dm_sync_ui.h"
#include "../host/dm_i18n.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
enum { CLOSED, HOME, CONNECT, HISTORY, DETAIL, CONFIRM, NOTICE };
static ds_ui_view v;
static cJSON *state,*records,*detail;
static _Atomic int dirty=1,want_open,shown;
static pthread_mutex_t paste_mu=PTHREAD_MUTEX_INITIALIZER;
static char *paste_result,*connection;
static int paste_waiting,paste_complete,page,selection,history_start,history_next=-1,history_offset,history_selection;
static _Atomic int paste_pending;
static unsigned paste_epoch;
static char import_hash[65];
static int confirmation_parent,notice_parent;static char command[32],revision[65],head[65],hash[65],notice[1024];
static char notice_key[256],banner[1024];static double banner_until;
static uint64_t waiting;static unsigned epoch,waiting_epoch;static int waiting_kind;
static const char *str(const cJSON *o,const char *k){const cJSON *x=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(x)?x->valuestring:"";}
static cJSON *get(const cJSON *o,const char *k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static int eq(const char *a,const char *b){return !strcmp(a,b);}
static void copy(char *d,size_t n,const char *s){snprintf(d,n,"%s",s?s:"");}
static void add(char *d,size_t n,const char *fmt,...){size_t z=strlen(d);if(z>=n-1)return;va_list ap;va_start(ap,fmt);vsnprintf(d+z,n-z,fmt,ap);va_end(ap);}
static int busy(void){return cJSON_IsTrue(get(state,"busy"))||waiting||paste_waiting;}
static void summary(char *out,size_t n,const cJSON *r){
    out[0]=0;if(!cJSON_IsObject(r)){copy(out,n,dm_ui("dmSyncNoProgress"));return;}
    const cJSON *dev=get(r,"device");const char *name=cJSON_IsObject(dev)?str(dev,"name"):"";
    if(*name)add(out,n,"%s\n",name);
    cJSON *at=get(r,"savedAt");if(cJSON_IsNumber(at)){time_t t=(time_t)(at->valuedouble/1000);struct tm tm;char stamp[64];
        if(localtime_r(&t,&tm)&&strftime(stamp,sizeof stamp,"%Y-%m-%d %H:%M:%S",&tm))add(out,n,"%s\n",stamp);}
    const cJSON *heroes=get(r,"characters");cJSON *h;
    cJSON_ArrayForEach(h,heroes)add(out,n,dm_ui("dmSyncHeroAndroid"),str(h,"name"),(int)cJSON_GetNumberValue(get(h,"level")));
    if(!cJSON_GetArraySize(heroes))add(out,n,"%s",dm_ui("dmSyncNoCharacters"));
}
// 列表只显示两行摘要；完整设备、多角色及冲突信息在详情页展示。
// Keep list captions to two lines; show full device, character and conflict details on the detail page.
static void caption(char *out,size_t n,const cJSON *r){
    out[0]=0;cJSON *at=get(r,"savedAt");char stamp[64]="";
    if(cJSON_IsNumber(at)){time_t t=(time_t)(at->valuedouble/1000);struct tm tm;if(localtime_r(&t,&tm))strftime(stamp,sizeof stamp,"%Y-%m-%d %H:%M",&tm);}
    add(out,n,"%s%s\n",cJSON_IsTrue(get(r,"uiConflict"))?"! ":"",stamp);
    const cJSON *heroes=get(r,"characters"),*hero=cJSON_GetArrayItem(heroes,0);
    if(!hero){add(out,n,"%s",dm_ui("dmSyncNoCharacters"));return;}
    const char *name=str(hero,"name");char short_name[96];size_t bytes=0;int units=0;
    for(const unsigned char *p=(const unsigned char*)name;*p;){int step=*p<0x80?1:(*p&0xe0)==0xc0?2:(*p&0xf0)==0xe0?3:4;
        int width=*p<0x80?1:2;if(units+width>16)break;memcpy(short_name+bytes,p,step);bytes+=step;p+=step;units+=width;}
    short_name[bytes]=0;if(name[bytes])strcat(short_name,"…");add(out,n,dm_ui("dmSyncHeroAndroid"),short_name,(int)cJSON_GetNumberValue(get(hero,"level")));
    size_t len=strlen(out);if(len&&out[len-1]=='\n')out[len-1]=0;
    int others=cJSON_GetArraySize(heroes)-1;if(others)add(out,n," (+%d)",others);
}
static void btn(const char *a,const char *key,int enabled){if(v.count>=DS_UI_ACTIONS)return;ds_ui_button *b=&v.buttons[v.count++];copy(b->action,sizeof b->action,a);copy(b->label,sizeof b->label,dm_ui(key));b->enabled=enabled;}
static void changed_page(int to){page=to;selection=0;v.generation++;}
static void message(const char *key,int back){copy(notice,sizeof notice,dm_ui(key));notice_parent=back;changed_page(NOTICE);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
const char *dm_sync_ui_notice(void){return now()<banner_until?banner:"";}
const char *dm_sync_ui_status_text(void){return dm_r2_message(str(state,"status"));}
static void refresh(void){char *s=dm_r2_status();cJSON *n=s?cJSON_Parse(s):NULL;free(s);if(n){cJSON_Delete(state);state=n;
        const char *code=str(state,"status");
        if(eq(code,"conflict")||eq(code,"restart_required")||eq(code,"unauthorized")||eq(code,"pending_upload")||eq(code,"pending_download")||eq(code,"connection_file_ready")){
            char key[256];snprintf(key,sizeof key,"%s:%s:%s",code,str(get(state,"remote"),"id"),str(get(state,"local"),"contentHash"));
            if(strcmp(key,notice_key)){copy(notice_key,sizeof notice_key,key);copy(banner,sizeof banner,dm_r2_message(code));banner_until=now()+5;}
        }
    }}
void dm_sync_ui_changed(void){atomic_store(&dirty,1);}
void dm_sync_ui_open(void){atomic_store(&want_open,1);}
int dm_sync_ui_active(void){return atomic_load(&shown);}
void dm_sync_ui_close(void){
    changed_page(CLOSED);atomic_store(&shown,0);epoch++;waiting=0;paste_waiting=0;
    ds_secret_free(connection);connection=NULL;cJSON_Delete(detail);detail=NULL;
}
static void pasted(char *text){pthread_mutex_lock(&paste_mu);ds_secret_free(paste_result);paste_result=text;paste_complete=1;pthread_mutex_unlock(&paste_mu);}
static void begin_confirm(const char *act,const char *id){
    copy(command,sizeof command,act);copy(revision,sizeof revision,id);confirmation_parent=page;
    copy(head,sizeof head,str(get(state,"remote"),"id"));copy(hash,sizeof hash,str(get(state,"local"),"contentHash"));
    changed_page(CONFIRM);selection=0;
}
static void request(const char *a,const char *id){
    uint64_t q=dm_r2_submit(a,id,head,hash);if(!q){message("dmSyncBusy",page);return;}
    waiting=q;waiting_epoch=epoch;waiting_kind=eq(a,"history")?1:2;atomic_store(&dirty,1);
}
static void history_request(int offset){char n[32];snprintf(n,sizeof n,"%d",offset);history_offset=offset;request("history",n);}
static void load_history(void){
    cJSON *h=get(state,"history");if(!cJSON_IsObject(h)){message("dmSyncHistoryRetry",HISTORY);return;}
    cJSON_Delete(records);records=cJSON_CreateArray();
    const char *keys[]={"conflicts","items"};for(int k=0;k<2;k++){cJSON *r;cJSON_ArrayForEach(r,get(h,keys[k])){
        int exists=0;cJSON *old;cJSON_ArrayForEach(old,records)if(eq(str(old,"id"),str(r,"id")))exists=1;
        if(exists)continue;cJSON *record=cJSON_Duplicate(r,1);cJSON_AddBoolToObject(record,"uiConflict",k==0);cJSON_AddItemToArray(records,record);
    }}
    cJSON *next=get(h,"next");history_next=cJSON_IsNumber(next)?(int)next->valuedouble:-1;history_start=0;changed_page(HISTORY);
}
void dm_sync_ui_tick(void){
    if(atomic_exchange(&want_open,0)){if(!page){epoch++;refresh();changed_page(HOME);atomic_store(&shown,1);}}
    if(atomic_exchange(&dirty,0))refresh();
    pthread_mutex_lock(&paste_mu);char *code=NULL;int completed=paste_complete;
    if(completed){code=paste_result;paste_result=NULL;paste_complete=0;}pthread_mutex_unlock(&paste_mu);
    if(completed){atomic_store(&paste_pending,0);if(page==CONNECT&&paste_waiting&&paste_epoch==epoch){paste_waiting=0;char *info=dm_r2_connection_preview(code);
        if(info){free(info);ds_secret_free(connection);connection=code;code=NULL;begin_confirm("connect","");}
        else message("dmSyncConnectionError",CONNECT);}ds_secret_free(code);}
    if(waiting&&(uint64_t)cJSON_GetNumberValue(get(state,"request_done"))>=waiting){
        int kind=waiting_kind;waiting=0;if(waiting_epoch==epoch&&page){
            if(eq(str(state,"status"),"confirmation_changed"))message("dmSyncConfirmationChanged",HOME);
            else if(kind==1&&page==HISTORY)load_history();
            else if(kind==2&&page==CONFIRM)changed_page(HOME);
        }
    }
}
void dm_sync_ui_action(const char *a){
    if(!page||!a)return;
    if(eq(a,"back")){
        if(page==HOME){dm_sync_ui_close();return;}
        if(page==CONFIRM){ds_secret_free(connection);connection=NULL;changed_page(confirmation_parent);return;}
        if(page==NOTICE){changed_page(notice_parent);return;}
        if(page==DETAIL){changed_page(HISTORY);selection=history_selection;return;}
        epoch++;waiting=0;paste_waiting=0;changed_page(HOME);return;
    }
    if(eq(a,"activate")){const ds_ui_view *view=dm_sync_ui_view();if(selection<view->count&&view->buttons[selection].enabled){char action[64];copy(action,sizeof action,view->buttons[selection].action);dm_sync_ui_action(action);}return;}
    if(eq(a,"cancel-download")){dm_r2_cancel();return;}
    if(busy())return;
    if(eq(a,"connect-page")){changed_page(CONNECT);return;}
    if(eq(a,"paste")){if(atomic_exchange(&paste_pending,1))return;paste_waiting=1;paste_epoch=epoch;ds_platform_paste(pasted);return;}
    if(eq(a,"import")){char *info=dm_r2_connection_file();if(!info){message("dmSyncConnectionError",CONNECT);return;}cJSON *meta=cJSON_Parse(info);copy(import_hash,sizeof import_hash,str(meta,"sourceHash"));cJSON_Delete(meta);free(info);begin_confirm("import","");return;}
    if(eq(a,"sync")){request("sync","");return;}
    if(eq(a,"history")){changed_page(HISTORY);history_request(0);return;}
    if(eq(a,"retry-history")){history_request(history_offset);return;}
    if(eq(a,"next")){if(history_start+8<cJSON_GetArraySize(records)){history_start+=8;selection=0;v.generation++;}
        else if(history_next>=0)history_request(history_next);return;}
    if(eq(a,"previous")){if(history_start>=8){history_start-=8;selection=0;v.generation++;}else if(history_offset>0)history_request(history_offset>=50?history_offset-50:0);return;}
    if(!strncmp(a,"record-",7)){
        if(page!=HISTORY)return;int index=0;cJSON *r;
        // 按不可变版本编号查找，重排或重复标题不会把操作指向另一份进度。
        // Resolve immutable revision IDs, never list positions or duplicate display captions.
        cJSON_ArrayForEach(r,records){if(eq(str(r,"id"),a+7)){history_selection=index-history_start;cJSON_Delete(detail);detail=cJSON_Duplicate(r,1);changed_page(DETAIL);return;}index++;}return;
    }
    if(eq(a,"restore")){begin_confirm(a,str(detail,"id"));return;}
    if(eq(a,"local")||eq(a,"cloud")||eq(a,"confirm-empty")||eq(a,"disconnect")){begin_confirm(a,"");return;}
    if(eq(a,"confirm")){
        if(page!=CONFIRM)return;
        if(eq(command,"connect")){int rc=dm_r2_connect(connection);ds_secret_free(connection);connection=NULL;message(rc?"dmSyncConnectionError":"dmSyncConnectionReady",rc?CONNECT:HOME);}
        else if(eq(command,"import")){char *info=dm_r2_connection_file();cJSON *meta=info?cJSON_Parse(info):NULL;free(info);
            int same=eq(import_hash,str(meta,"sourceHash"));cJSON_Delete(meta);if(!same){message("dmSyncConfirmationChanged",CONNECT);return;}
            int rc=dm_r2_import_connection();message(rc<0?"dmSyncConnectionError":rc?"dmSyncImportCleanup":"dmSyncConnectionReady",rc<0?CONNECT:HOME);}
        else if(eq(command,"disconnect")){if(dm_r2_disconnect())message("dmSyncConnectionError",HOME);else changed_page(HOME);}
        else request(command,revision);
    }
}
void dm_sync_ui_select(int d){const ds_ui_view *view=dm_sync_ui_view();int n=view->count;if(!n)return;
    int step=d<0?-1:1;if(d==2||d== -2){int target=selection+d;if(target>=0&&target<n&&view->buttons[target].enabled){selection=target;return;}}
    for(int i=0;i<n;i++){selection=(selection+n+step)%n;if(view->buttons[selection].enabled)break;}}
const ds_ui_view *dm_sync_ui_view(void){
    unsigned generation=v.generation;memset(&v,0,sizeof v);v.generation=generation;v.page=page;
    copy(v.title,sizeof v.title,dm_ui("dmSyncTitle"));copy(v.status,sizeof v.status,dm_r2_message(str(state,"status")));
    int idle=!busy();const char *status=str(state,"status");
    if(eq(status,"downloading")){double total=cJSON_GetNumberValue(get(state,"total")),got=cJSON_GetNumberValue(get(state,"received"));
        snprintf(v.status,sizeof v.status,dm_ui(total>0?"dmSyncDownloadPercentAndroid":"dmSyncDownloadKBAndroid"),total>0?(int)(100*got/total):(int)(got/1024));}
    if(page==HOME){v.columns=1;
        copy(v.left,sizeof v.left,dm_ui("dmSyncLocalTitle"));add(v.left,sizeof v.left,"\n");char b[DS_UI_TEXT];summary(b,sizeof b,get(state,"local"));add(v.left,sizeof v.left,"%s",b);
        copy(v.right,sizeof v.right,dm_ui("dmSyncCloudTitle"));add(v.right,sizeof v.right,"\n");summary(b,sizeof b,get(state,"remote"));add(v.right,sizeof v.right,"%s",b);
        if(!cJSON_IsTrue(get(state,"connected")))btn("connect-page","dmSyncConnectDevice",idle);
        else {btn("sync","dmSyncSyncNow",idle);if(eq(status,"conflict")||eq(status,"confirmation_changed")){btn("local","dmSyncUseLocal",idle);btn("cloud","dmSyncUseCloud",idle);}
            if(eq(status,"confirm_empty"))btn("confirm-empty","dmSyncDeleteLast",idle);
            btn("history","dmSyncHistory",idle);btn("disconnect","dmSyncDisconnect",idle);}
        if(eq(status,"downloading"))btn("cancel-download","dmSyncCancelOffline",1);
        btn("back","dmSyncLaterClose",1);
    }else if(page==CONNECT){copy(v.left,sizeof v.left,dm_ui("dmSyncConnectHint"));btn("paste","dmSyncPasteCode",idle);
        char *info=dm_r2_connection_file();if(info){free(info);btn("import","dmSyncImportFile",idle);}btn("back","dmSyncBack",1);
    }else if(page==HISTORY){copy(v.title,sizeof v.title,dm_ui("dmSyncHistoryConflicts"));int n=cJSON_GetArraySize(records);
        if(!n)copy(v.left,sizeof v.left,dm_ui(waiting?"dmSyncChecking":"dmSyncHistoryEmpty"));
        for(int i=history_start;i<n&&i<history_start+8;i++){cJSON *r=cJSON_GetArrayItem(records,i);char a[64];snprintf(a,sizeof a,"record-%s",str(r,"id"));btn(a,"dmSyncRestore",idle);
            ds_ui_button *b=&v.buttons[v.count-1];caption(b->label,sizeof b->label,r);}
        if(history_start||history_offset)btn("previous","dmSyncPreviousPage",idle);
        if(history_start+8<n||history_next>=0)btn("next","dmSyncNextPage",idle);
        btn("retry-history","dmSyncRefresh",idle);btn("back","dmSyncBack",1);
    }else if(page==DETAIL){copy(v.title,sizeof v.title,dm_ui("dmSyncHistory"));summary(v.left,sizeof v.left,detail);
        if(cJSON_IsTrue(get(detail,"uiConflict")))add(v.left,sizeof v.left,"\n%s",dm_ui("dmSyncConflict"));btn("restore","dmSyncRestore",idle);btn("back","dmSyncBack",1);
    }else if(page==CONFIRM){copy(v.title,sizeof v.title,dm_ui(eq(command,"disconnect")?"dmSyncDisconnect":eq(command,"confirm-empty")?"dmSyncDeleteAllQuestion":"dmSyncSwitchQuestion"));
        const char *key=eq(command,"disconnect")?"dmSyncDisconnectHint":eq(command,"confirm-empty")?"dmSyncDeleteAllHint":eq(command,"local")?"dmSyncKeepCloud":eq(command,"cloud")?"dmSyncKeepLocal":"dmSyncRestoreHint";
        copy(v.left,sizeof v.left,dm_ui(key));
        if(eq(command,"connect")||eq(command,"import")){char *info=eq(command,"connect")?dm_r2_connection_preview(connection):dm_r2_connection_file();cJSON *c=info?cJSON_Parse(info):NULL;free(info);
            copy(v.title,sizeof v.title,dm_ui("dmSyncConnectDevice"));snprintf(v.left,sizeof v.left,dm_ui("dmSyncImportHint"),str(c,"url"));add(v.left,sizeof v.left,"\n%s",str(c,"device"));cJSON_Delete(c);
        }else if(eq(command,"restore")){char b[DS_UI_TEXT];summary(b,sizeof b,detail);add(v.left,sizeof v.left,"\n\n%s",b);}
        btn("back","dmSyncCancel",1);btn("confirm","dmSyncConfirm",idle);
    }else if(page==NOTICE){copy(v.left,sizeof v.left,notice);btn("back","dmSyncOK",1);}
    if(selection>=v.count)selection=v.count?v.count-1:0;v.selection=selection;return &v;
}
