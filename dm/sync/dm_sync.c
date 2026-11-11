/* Personal cloud saves: a serialized network worker and immutable committed snapshots.
 * The game hook only clones; no network, hashing, ZIP creation or UI under its lock. */
#include "dm_sync.h"
#include "../host/dm_i18n.h"
#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

__attribute__((weak)) int ds_transport(const char *m,const char *u,const char *t,const void *b,size_t n,unsigned ms,unsigned char **o,size_t *z){(void)m;(void)u;(void)t;(void)b;(void)n;(void)ms;*o=NULL;*z=0;return -1;}
__attribute__((weak)) char *ds_credentials_load(void){return NULL;}
__attribute__((weak)) int ds_credentials_store(const char *s){(void)s;return -1;}
__attribute__((weak)) void dm_sync_ui_changed(void){}
__attribute__((weak)) void ds_platform_changed(void){dm_sync_ui_changed();}
__attribute__((weak)) void ds_platform_show(void){}
__attribute__((weak)) void dm_sync_ui_open(void){ds_platform_show();}
__attribute__((weak)) int dm_sync_ui_ready(void){return 0;}
__attribute__((weak)) void ds_platform_paste(void (*complete)(char *)){complete(NULL);}
__attribute__((weak)) void ds_transport_cancel(void){}

static pthread_mutex_t qmu=PTHREAD_MUTEX_INITIALIZER,cmu=PTHREAD_MUTEX_INITIALIZER,vmu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static _Atomic int enabled,session,background,cancelled,busy,booted;
static int queued,preview_pending;
static uint64_t request_seq, queued_request, active_request, done_request;
static char queued_head[65], queued_hash[65];
static _Thread_local char checked_head[65], checked_hash[65];
/* 禁止晚到确认操作作用于另一份进度。
 * Never let a delayed confirmation act on a different save revision. */
static int checked;
static char action[32],argument[65],savepath[1536],root[1600],endpoint[600],token[128],device[40];
static cJSON *state,*view;
// 启动检查最多等 5 秒；后台和明确的用户操作允许较慢网络完成请求。
// Startup checks wait at most five seconds; background/user requests allow slower networks to finish.
static _Thread_local unsigned json_timeout_ms=30000;
static const char *str(const cJSON *o,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static cJSON *item(const cJSON *o,const char *k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static void replace(cJSON *o,const char *k,cJSON *v){cJSON_DeleteItemFromObjectCaseSensitive(o,k);cJSON_AddItemToObject(o,k,v);}
static void setstr(cJSON *o,const char *k,const char *s){replace(o,k,cJSON_CreateString(s?s:""));}
static int same(const char *a,const char *b){return !strcmp(a,b);}
static void path(char out[2048],const char *name){snprintf(out,2048,"%s/%s",root,name);}
static cJSON *readjson(const char *name){char p[2048];path(p,name);size_t n;unsigned char *b=ds_read(p,&n);cJSON *j=b?cJSON_ParseWithLength((char *)b,n):NULL;free(b);return j;}
static int writejson(const char *name,const cJSON *j){char p[2048];path(p,name);char *s=cJSON_PrintUnformatted(j);int r=s?ds_write(p,s,strlen(s)):-1;free(s);return r;}
static int persist(void){return writejson("state.json",state);}
static void status(const char *code,const cJSON *local,const cJSON *remote){
    pthread_mutex_lock(&vmu);if(!view)view=cJSON_CreateObject();setstr(view,"status",code);
    replace(view,"connected",cJSON_CreateBool(atomic_load(&enabled)));
    if(local)replace(view,"local",cJSON_Duplicate(local,1));
    if(remote)replace(view,"remote",cJSON_Duplicate(remote,1));
    pthread_mutex_unlock(&vmu);ds_platform_changed();
#ifdef DM_SYNC_TESTING
    if(getenv("DM_SYNC_TEST")&&root[0]){char p[2048];path(p,"status.json");char *s=dm_r2_status();if(s){ds_write(p,s,strlen(s));free(s);}}
#endif
}
char *dm_r2_status(void){
    pthread_mutex_lock(&vmu);cJSON *v=view?cJSON_Duplicate(view,1):cJSON_CreateObject();pthread_mutex_unlock(&vmu);
    if(!item(v,"status")){setstr(v,"status","disconnected");replace(v,"connected",cJSON_CreateFalse());}
    pthread_mutex_lock(&qmu);
    replace(v,"busy",cJSON_CreateBool(queued||atomic_load(&busy)));
    replace(v,"request_id",cJSON_CreateNumber((double)(queued_request?queued_request:active_request)));
    replace(v,"request_done",cJSON_CreateNumber((double)done_request));
    pthread_mutex_unlock(&qmu);char *s=cJSON_PrintUnformatted(v);cJSON_Delete(v);return s;
}
void dm_r2_progress(size_t received,size_t total){
    int changed=0;pthread_mutex_lock(&vmu);
    if(view&&same(str(view,"status"),"downloading")){
        replace(view,"received",cJSON_CreateNumber((double)received));replace(view,"total",cJSON_CreateNumber((double)total));changed=1;}
    pthread_mutex_unlock(&vmu);if(changed)ds_platform_changed();
}
static void removefile(const char *name){char p[2048];path(p,name);unlink(p);}
static int request(const char *method,const char *route,const void *data,size_t n,unsigned timeout,unsigned char **out,size_t *size){
    if(!atomic_load(&enabled)||atomic_load(&cancelled))return -1;
    char url[1024];if(snprintf(url,sizeof url,"%s%s",endpoint,route)>=(int)sizeof url)return -1;
    int code=ds_transport(method,url,token,data,n,timeout,out,size);
    // 仅记录接口路径与状态码，便于排查连通性；令牌和存档正文不写日志。
    // Log only route/status for connectivity diagnosis; never log credentials or save payloads.
    if(code<0||code>=400)fprintf(stderr,"[r2] %s %s status=%d\n",method,route,code);
    return code;
}
static cJSON *jsonrequest(const char *method,const char *route,const cJSON *body,int *code){
    char *data=body?cJSON_PrintUnformatted(body):NULL;unsigned char *out=NULL;size_t n=0;
    *code=request(method,route,data,data?strlen(data):0,json_timeout_ms,&out,&n);free(data);
    cJSON *j=out?cJSON_ParseWithLength((char *)out,n):NULL;free(out);return j;
}
static int current_snapshot(int boot,unsigned char **zip,size_t *n,cJSON **manifest){
    char src[2048],tmp[2048],id[33];if(boot)return ds_snapshot(savepath,device,str(state,"base"),zip,n,manifest);
    ds_uuid(id);path(src,"capture");snprintf(tmp,sizeof tmp,"%s/read-%s",root,id);
    pthread_mutex_lock(&cmu);int rc=clonefile(src,tmp,0);pthread_mutex_unlock(&cmu);
    if(rc)return -1;rc=ds_snapshot(tmp,device,str(state,"base"),zip,n,manifest);ds_remove(tmp);return rc;
}
static void base(const cJSON *rev){setstr(state,"base",str(rev,"id"));setstr(state,"hash",str(rev,"contentHash"));}
static int apply(const cJSON *head,const cJSON *local,int boot){
    char route[100],p[2048];snprintf(route,sizeof route,"/v1/snapshots/%s",str(head,"snapshot"));
    unsigned char *bytes=NULL;size_t n=0;status("downloading",local,head);
    int http=request("GET",route,NULL,0,60000,&bytes,&n);cJSON *m=NULL;
    if(http!=200||atomic_load(&cancelled)){free(bytes);status(http==401?"unauthorized":"pending_download",local,head);return -1;}
    if(ds_verify(bytes,n,&m)||!same(str(m,"contentHash"),str(head,"contentHash"))){free(bytes);cJSON_Delete(m);status("invalid_download",local,head);return -1;}
    cJSON_Delete(m);path(p,"download.zip");if(ds_write(p,bytes,n)){free(bytes);status("disk_error",local,head);return -1;}
    if(!boot){free(bytes);status("restart_required",local,head);return 0;}
    cJSON *journal=cJSON_CreateObject();cJSON_AddItemToObject(journal,"revision",cJSON_Duplicate(head,1));setstr(journal,"before",str(local,"contentHash"));
    if(writejson("install.json",journal)){cJSON_Delete(journal);free(bytes);status("disk_error",local,head);return -1;}cJSON_Delete(journal);
    char backups[2048];path(backups,"backups");int rc=ds_install(bytes,n,savepath,backups);free(bytes);
    if(rc){status("disk_error",local,head);return -1;}
#ifdef DM_SYNC_TEST_DRIVER
    if(getenv("DM_SYNC_CRASH_AFTER_SWAP"))_exit(86);
#endif
    base(head);cJSON_DeleteItemFromObjectCaseSensitive(state,"adopt");if(persist()){status("disk_error",local,head);return -1;}
    removefile("install.json");removefile("download.zip");status("synced",head,head);return 0;
}
/* Receipt is persisted before a request, so a lost response can safely be retried. */
static int pending_send(void){
    cJSON *p=readjson("pending.json");if(!p)return 0;
    int code=0;cJSON *reply=NULL;const char *route=str(p,"route");cJSON *body=item(p,"body");
    cJSON *summary=item(p,"local");
    if(!same(route,"/v1/resolve")){
        char file[2048],url[100];path(file,"pending.zip");size_t n;unsigned char *b=ds_read(file,&n),*out=NULL;size_t z=0;
        if(!b){cJSON_Delete(p);status("disk_error",NULL,NULL);return -1;}
        snprintf(url,sizeof url,"/v1/snapshots/%s",str(body,"snapshot"));
        code=request("PUT",url,b,n,30000,&out,&z);free(b);free(out);
        if(code!=200&&code!=201){cJSON_Delete(p);status(code==401?"unauthorized":code==422?"invalid_local":"pending_upload",NULL,NULL);return -1;}
    }
    reply=jsonrequest("POST",route,body,&code);
    if((code==200||code==201)&&item(reply,"revision")){
        cJSON *rev=item(reply,"revision");
        if(same(route,"/v1/resolve")&&!same(str(p,"localHash"),str(rev,"contentHash"))){
            cJSON *adopt=cJSON_CreateObject();setstr(adopt,"expectedLocalHash",str(p,"localHash"));cJSON_AddItemToObject(adopt,"revision",cJSON_Duplicate(rev,1));replace(state,"adopt",adopt);
        }else base(rev);
        cJSON_DeleteItemFromObjectCaseSensitive(state,"conflict");
        if(persist()){cJSON_Delete(reply);cJSON_Delete(p);status("disk_error",NULL,NULL);return -1;}
        removefile("pending.json");removefile("pending.zip");status(item(state,"adopt")?"restart_required":"synced",NULL,rev);
        cJSON_Delete(reply);cJSON_Delete(p);return 0;
    }
    const char *error=str(reply,"error");
    if(code==409&&same(error,"conflict")){
        setstr(state,"conflict",str(reply,"conflict"));if(!persist()){removefile("pending.json");removefile("pending.zip");}
        status("conflict",summary,item(reply,"head"));
    }else if(code==409&&same(error,"confirm_empty_required")){
        unsigned char *latest=NULL;size_t size=0;cJSON *current=NULL;
        if(!current_snapshot(0,&latest,&size,&current)&&!same(str(current,"contentHash"),str(p,"localHash"))){
            /* A new character was created after this deletion was queued. The
             * server confirmed the old operation never published, so replace
             * that obsolete intent with the current complete snapshot. */
            free(latest);cJSON_Delete(current);removefile("pending.json");removefile("pending.zip");cJSON_Delete(reply);cJSON_Delete(p);return 0;
        }
        free(latest);cJSON_Delete(current);status("confirm_empty",summary,NULL);
    }
    else if(code==410){removefile("pending.json");removefile("pending.zip");status("pending_upload",NULL,NULL);}
    else if(code==409&&same(error,"head_changed")){removefile("pending.json");status("conflict",NULL,item(reply,"head"));}
    else status(code==401?"unauthorized":code==422?"incompatible":"pending_upload",NULL,NULL);
    cJSON_Delete(reply);cJSON_Delete(p);return -1;
}
static int enqueue_upload(const unsigned char *zip,size_t n,const cJSON *local,const char *parent,int confirmed,int defer){
    cJSON *p=cJSON_CreateObject(),*body=cJSON_AddObjectToObject(p,"body");char sid[33],op[33],file[2048];ds_uuid(sid);ds_uuid(op);
    setstr(p,"route","/v1/commit");setstr(p,"localHash",str(local,"contentHash"));setstr(body,"operation",op);setstr(body,"snapshot",sid);
    cJSON_AddItemToObject(p,"local",cJSON_Duplicate(local,1));
    cJSON_AddItemToObject(body,"base",*parent?cJSON_CreateString(parent):cJSON_CreateNull());cJSON_AddBoolToObject(body,"confirmEmpty",confirmed);
    path(file,"pending.zip");int rc=ds_write(file,zip,n)||writejson("pending.json",p);cJSON_Delete(p);
    if(rc){status("disk_error",local,NULL);return -1;}
    if(defer){status("pending_upload",local,NULL);return 0;}
    return pending_send();
}
static void sync_run(int boot){
    if(!atomic_load(&enabled)||atomic_load(&cancelled))return;
    /* A cold launch never waits for an upload. A durable receipt also prevents
     * pulling an older head over local changes while a commit response is lost. */
    if(boot){cJSON *pending=readjson("pending.json");if(pending){cJSON_Delete(pending);status("pending_upload",NULL,NULL);return;}}
    else if(pending_send())return;
    unsigned char *zip=NULL;size_t n=0;cJSON *local=NULL;
    if(current_snapshot(boot,&zip,&n,&local)){status("invalid_local",NULL,NULL);return;}
    /* Recover a swap that completed before its state update. */
    cJSON *journal=readjson("install.json");
    if(journal&&same(str(local,"contentHash"),str(item(journal,"revision"),"contentHash"))){
        base(item(journal,"revision"));if(persist()){cJSON_Delete(journal);status("disk_error",local,NULL);free(zip);cJSON_Delete(local);return;}removefile("install.json");}
    cJSON_Delete(journal);
    int code;cJSON *remote=jsonrequest("GET","/v1/status",NULL,&code),*head=item(remote,"head");
    if(code!=200||!remote){status(code==401?"unauthorized":"offline",local,NULL);goto done;}
    if(strcmp(str(remote,"game"),DS_GAME)||!cJSON_IsNumber(item(remote,"format"))||item(remote,"format")->valuedouble!=1||
       !cJSON_IsNumber(item(remote,"protocol"))||item(remote,"protocol")->valuedouble!=1||
       !(cJSON_IsNull(head)||(cJSON_IsObject(head)&&strlen(str(head,"id"))==32&&strlen(str(head,"snapshot"))==32&&strlen(str(head,"contentHash"))==64))){status("incompatible",local,NULL);goto done;}
    if(cJSON_IsNull(head)){
        if(cJSON_GetArraySize(item(local,"files"))>0)enqueue_upload(zip,n,local,"",0,boot);
        else status("empty",local,NULL);goto done;
    }
    if(same(str(local,"contentHash"),str(head,"contentHash"))){
        base(head);cJSON_DeleteItemFromObjectCaseSensitive(state,"conflict");cJSON_DeleteItemFromObjectCaseSensitive(state,"adopt");
        if(persist())status("disk_error",local,head);else status("synced",local,head);goto done;
    }
    cJSON *adopt=item(state,"adopt");
    if(adopt&&same(str(adopt,"expectedLocalHash"),str(local,"contentHash"))){apply(head,local,boot);goto done;}
    int first=!str(state,"base")[0],empty=!cJSON_GetArraySize(item(local,"files"));
    if((first&&empty)||same(str(local,"contentHash"),str(state,"hash"))){apply(head,local,boot);goto done;}
    if(str(state,"conflict")[0]){status("conflict",local,head);goto done;}
    status("uploading",local,head);
    enqueue_upload(zip,n,local,str(state,"base"),0,boot);
done:free(zip);cJSON_Delete(local);cJSON_Delete(remote);
}
static void resolve_run(const char *which,const char *id){
    cJSON *pending=readjson("pending.json");
    if(pending){cJSON_Delete(pending);pending_send();pending=readjson("pending.json");if(pending){cJSON_Delete(pending);return;}}
    unsigned char *zip=NULL;size_t n=0;cJSON *local=NULL;
    if(current_snapshot(0,&zip,&n,&local)){status("invalid_local",NULL,NULL);return;}
    int code;cJSON *remote=jsonrequest("GET","/v1/status",NULL,&code);
    if(code!=200){status("offline",local,NULL);goto done;}
    cJSON *head=item(remote,"head");if(!cJSON_IsObject(head))goto done;
            if(checked&&!same(checked_head,str(head,"id"))){status("confirmation_changed",local,head);goto done;}
    if(checked&&(!same(checked_head,str(head,"id"))||!same(checked_hash,str(local,"contentHash")))){
        status("confirmation_changed",local,head);goto done;
    }
    /* The player may have continued playing after postponing a conflict.
     * Preserve that latest committed local copy in the cloud before selecting
     * another version. The ordinary durable receipt protects this step too. */
    if(!same(which,"local")&&!same(str(local,"contentHash"),str(head,"contentHash"))){
        int preserved=0;cJSON *r;
        cJSON_ArrayForEach(r,item(remote,"conflicts"))if(same(str(r,"contentHash"),str(local,"contentHash")))preserved=1;
        if(!preserved){
            enqueue_upload(zip,n,local,"",0,0);
            cJSON *left=readjson("pending.json");if(left){cJSON_Delete(left);goto done;}
            cJSON_Delete(remote);remote=jsonrequest("GET","/v1/status",NULL,&code);
            if(code!=200){status("offline",local,NULL);goto done;}
            head=item(remote,"head");if(!cJSON_IsObject(head))goto done;
        }
    }
    char sid[65]="";
    if(same(which,"local")){
        char url[100];ds_uuid(sid);snprintf(url,sizeof url,"/v1/snapshots/%s",sid);
        unsigned char *out=NULL;size_t sz=0;code=request("PUT",url,zip,n,30000,&out,&sz);free(out);
        if(code!=200&&code!=201){status("pending_upload",local,head);goto done;}
    }else if(same(which,"cloud"))snprintf(sid,sizeof sid,"%s",str(head,"snapshot"));
    else{
        char route[128];size_t offset=0;int found=0;
        do {snprintf(route,sizeof route,"/v1/history?offset=%zu",offset);cJSON *h=jsonrequest("GET",route,NULL,&code);
            if(code!=200){cJSON_Delete(h);status(code==401?"unauthorized":"offline",local,head);goto done;}cJSON *r;
            cJSON_ArrayForEach(r,item(h,"items"))if(same(str(r,"id"),id)){snprintf(sid,sizeof sid,"%s",str(r,"snapshot"));found=1;}
            cJSON_ArrayForEach(r,item(h,"conflicts"))if(same(str(r,"id"),id)){snprintf(sid,sizeof sid,"%s",str(r,"snapshot"));found=1;}
            cJSON *next=item(h,"next");offset=cJSON_IsNumber(next)?(size_t)next->valuedouble:0;cJSON_Delete(h);
        }while(!found&&offset);
        if(!found){status("version_missing",local,head);goto done;}
    }
    cJSON *p=cJSON_CreateObject(),*body=cJSON_AddObjectToObject(p,"body"),*conflicts=cJSON_AddArrayToObject(body,"conflicts"),*r;char op[33];ds_uuid(op);
    setstr(p,"route","/v1/resolve");setstr(p,"localHash",str(local,"contentHash"));
    setstr(body,"operation",op);setstr(body,"snapshot",sid);setstr(body,"base",str(head,"id"));
    cJSON_ArrayForEach(r,item(remote,"conflicts"))cJSON_AddItemToArray(conflicts,cJSON_CreateString(str(r,"id")));
    if(!writejson("pending.json",p))pending_send();else status("disk_error",local,head);
    cJSON_Delete(p);
done:free(zip);cJSON_Delete(local);cJSON_Delete(remote);
}
static void history_run(const char *offset){
    char route[100];snprintf(route,sizeof route,"/v1/history?offset=%lu",strtoul(offset,NULL,10));int code;
    cJSON *h=jsonrequest("GET",route,NULL,&code);
    if(code==200){pthread_mutex_lock(&vmu);if(!view)view=cJSON_CreateObject();replace(view,"history",h);pthread_mutex_unlock(&vmu);ds_platform_changed();}
    else {cJSON_Delete(h);status("offline",NULL,NULL);}
}
static void preview_run(void){
    unsigned char *zip=NULL;size_t n=0;cJSON *local=NULL;
    if(current_snapshot(0,&zip,&n,&local)){status("invalid_local",NULL,NULL);return;}
    free(zip);
    /* Metadata refreshes after each committed save, without publishing during
     * gameplay. Keep a deferred conflict visible while the player continues. */
    char *s=dm_r2_status();cJSON *v=cJSON_Parse(s);free(s);
    const char *code=str(v,"status");
    if(str(state,"conflict")[0])code="conflict";
    else if(same(str(local,"contentHash"),str(state,"hash")))code="synced";
    else if(!same(code,"restart_required")&&!same(code,"confirm_empty")&&!same(code,"unauthorized"))code="local_saved";
    status(code,local,NULL);cJSON_Delete(v);cJSON_Delete(local);
}
static void *worker(void *unused){
    (void)unused;
    for(;;){pthread_mutex_lock(&qmu);while(!queued&&!preview_pending)pthread_cond_wait(&cond,&qmu);
        char a[32],id[65];checked=0;if(queued){strcpy(a,action);strcpy(id,argument);queued=0;
            active_request=queued_request;queued_request=0;checked=active_request!=0;
            strcpy(checked_head,queued_head);strcpy(checked_hash,queued_hash);
        }else{strcpy(a,"preview");id[0]=0;preview_pending=0;}
        atomic_store(&busy,1);pthread_mutex_unlock(&qmu);
        atomic_store(&cancelled,0);
        if(atomic_load(&enabled)){
            if(same(a,"preview"))preview_run();
            else if(same(a,"history"))history_run(id);
            else if(same(a,"local")||same(a,"cloud")||same(a,"restore"))resolve_run(a,id);
            else if(same(a,"confirm-empty")){cJSON *p=readjson("pending.json");if(p){
                int code=0;cJSON *remote=checked?jsonrequest("GET","/v1/status",NULL,&code):NULL;
                if(checked&&(code!=200||!same(checked_head,str(item(remote,"head"),"id"))||!same(checked_hash,str(item(p,"local"),"contentHash"))))status(code==200?"confirmation_changed":"offline",item(p,"local"),item(remote,"head"));
                else {replace(item(p,"body"),"confirmEmpty",cJSON_CreateBool(1));if(!writejson("pending.json",p))pending_send();}
                cJSON_Delete(remote);cJSON_Delete(p);}}
            else sync_run(0);
        }
        pthread_mutex_lock(&qmu);atomic_store(&busy,0);
        if(active_request)done_request=active_request;active_request=0;
        pthread_cond_broadcast(&cond);pthread_mutex_unlock(&qmu);ds_platform_changed();
    }return NULL;
}
void dm_r2_command(const char *a,const char *id){
    if(!atomic_load(&enabled))return;
    if(same(a,"history")){pthread_mutex_lock(&vmu);cJSON_DeleteItemFromObjectCaseSensitive(view,"history");pthread_mutex_unlock(&vmu);}
    pthread_mutex_lock(&qmu);
    /* Automatic events never replace an explicit queued user decision. */
    if((!queued||!same(a,"sync"))&&!queued_request){queued_request=0;queued_head[0]=queued_hash[0]=0;snprintf(action,sizeof action,"%s",a);snprintf(argument,sizeof argument,"%s",id?id:"");queued=1;}
    pthread_cond_signal(&cond);pthread_mutex_unlock(&qmu);
}
uint64_t dm_r2_submit(const char *a,const char *id,const char *head,const char *hash){
    if(!atomic_load(&enabled)||!a||
       !(same(a,"sync")||same(a,"history")||same(a,"local")||same(a,"cloud")||same(a,"restore")||same(a,"confirm-empty")))return 0;
    pthread_mutex_lock(&qmu);
    if(queued||atomic_load(&busy)){pthread_mutex_unlock(&qmu);return 0;}
    uint64_t request=++request_seq;queued_request=request;
    if(same(a,"history")){pthread_mutex_lock(&vmu);cJSON_DeleteItemFromObjectCaseSensitive(view,"history");pthread_mutex_unlock(&vmu);}
    snprintf(action,sizeof action,"%s",a);snprintf(argument,sizeof argument,"%s",id?id:"");
    snprintf(queued_head,sizeof queued_head,"%s",head?head:"");snprintf(queued_hash,sizeof queued_hash,"%s",hash?hash:"");
    queued=1;pthread_cond_signal(&cond);pthread_mutex_unlock(&qmu);ds_platform_changed();return request;
}
void dm_r2_on_commit(const char *dir){
    if(!atomic_load(&enabled))return;char tmp[2048],cap[2048];path(tmp,"capture.new");path(cap,"capture");
    pthread_mutex_lock(&cmu);ds_remove(tmp);int rc=clonefile(dir,tmp,0);
    if(!rc){struct stat st;rc=!lstat(cap,&st)?renamex_np(tmp,cap,RENAME_SWAP):rename(tmp,cap);ds_remove(tmp);}
    pthread_mutex_unlock(&cmu);
    if(rc){status("snapshot_failed",NULL,NULL);return;}
    if(!atomic_load(&session)||atomic_load(&background))dm_r2_command("sync","");
    else {pthread_mutex_lock(&qmu);preview_pending=1;pthread_cond_signal(&cond);pthread_mutex_unlock(&qmu);}
}
static void ingame(int on){if(on)atomic_store(&session,1);}
static void charsel(void){atomic_store(&session,0);dm_r2_command("sync","");}
int dm_r2_enabled(void){return atomic_load(&enabled);}
void dm_r2_flush(int bg){atomic_store(&background,bg!=0);dm_r2_command("sync","");}
void dm_r2_foreground(void){atomic_store(&background,0);if(!atomic_load(&session))dm_r2_command("sync","");}
int dm_r2_wait(double sec){struct timespec d={0,10000000};for(int i=0;i<(int)(sec*100);i++){
    pthread_mutex_lock(&qmu);int active=queued||preview_pending||atomic_load(&busy);pthread_mutex_unlock(&qmu);if(!active)return 1;nanosleep(&d,NULL);}return 0;}
void dm_r2_cancel(void){atomic_store(&cancelled,1);ds_transport_cancel();status("offline",NULL,NULL);}
void dm_r2_show(void){dm_sync_ui_open();}
static int valid_config(const cJSON *c){
    const char *u=str(c,"url"),*t=str(c,"token"),*d=str(c,"device");size_t n=strlen(u);
    int local=getenv("DM_SYNC_TEST")&&cJSON_IsTrue(item(c,"test"))&&!strncmp(u,"http://localhost:",17);
    if((strncmp(u,"https://",8)&&!local)||n<9||n>550||strchr(u,'@')||strchr(u,'?')||strchr(u,'#')||u[n-1]=='/'||strlen(t)!=43||strlen(d)!=32)return 0;
    for(const char *p=t;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return 0;
    for(const char *p=d;*p;p++)if(!((*p>='a'&&*p<='f')||(*p>='0'&&*p<='9')))return 0;return 1;
}
void ds_secret_free(char *s){if(s){volatile char *p=s;size_t n=strlen(s);while(n--)*p++=0;free(s);}}
char *dm_r2_connection_preview(const char *text){
    if(!text||strlen(text)>4096)return NULL;const char *end=NULL;cJSON *c=cJSON_ParseWithOpts(text,&end,1);
    int test=getenv("DM_SAVE_TEST")||getenv("DM_SAVE_FRESH")||getenv("DM_APP_SUPPORT");
    int ok=c&&valid_config(c)&&(test?(getenv("DM_SYNC_TEST")&&cJSON_IsTrue(item(c,"test"))):!cJSON_IsTrue(item(c,"test")));
    cJSON *v=ok?cJSON_CreateObject():NULL;
    if(v){char digest[65];ds_hash(text,strlen(text),digest);setstr(v,"url",str(c,"url"));setstr(v,"device",str(c,"device"));setstr(v,"sourceHash",digest);}
    cJSON *secret=item(c,"token");if(cJSON_IsString(secret))memset(secret->valuestring,0,strlen(secret->valuestring));
    cJSON_Delete(c);char *out=v?cJSON_PrintUnformatted(v):NULL;cJSON_Delete(v);return out;
}
// 连接文件只从应用存档目录旁读取；不跟随链接，不自动连接，也不回传令牌。
// Read only beside the app save directory; reject links, never auto-connect or expose a token.
static char *connection_read(char file[2048],struct stat *info){
    const char *sv=getenv("DM_SAVE");if(!sv||strlen(sv)>1450)return NULL;
    const char *slash=strrchr(sv,'/');if(!slash)return NULL;
    snprintf(file,2048,"%.*s/cloud-connection.json",(int)(slash-sv),sv);
    int fd=open(file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return NULL;
    if(fstat(fd,info)||!S_ISREG(info->st_mode)||info->st_nlink!=1||info->st_size<2||info->st_size>4096){close(fd);return NULL;}
    size_t size=(size_t)info->st_size,got=0;char *text=calloc(size+1,1);if(!text){close(fd);return NULL;}
    while(got<size){ssize_t n=read(fd,text+got,size-got);if(n<0&&errno==EINTR)continue;if(n<=0)break;got+=(size_t)n;}
    close(fd);const char *end=NULL;cJSON *c=got==size&&!memchr(text,0,size)?cJSON_ParseWithOpts(text,&end,1):NULL;
    int test=getenv("DM_SAVE_TEST")||getenv("DM_SAVE_FRESH")||getenv("DM_APP_SUPPORT");
    int ok=c&&valid_config(c)&&(test?(getenv("DM_SYNC_TEST")&&cJSON_IsTrue(item(c,"test"))):!cJSON_IsTrue(item(c,"test")));
    cJSON_Delete(c);if(!ok){ds_secret_free(text);return NULL;}return text;
}
char *dm_r2_connection_file(void){
    if(atomic_load(&enabled))return NULL;char file[2048];struct stat st;char *text=connection_read(file,&st);if(!text)return NULL;
    char *out=dm_r2_connection_preview(text);ds_secret_free(text);return out;
}
int dm_r2_import_connection(void){
    if(atomic_load(&enabled))return -1;char file[2048];struct stat before,after;char *text=connection_read(file,&before);if(!text)return -1;
    int rc=dm_r2_connect(text);char *stored=rc?NULL:ds_credentials_load();
    if(!rc&&(!stored||strcmp(stored,text)))rc=-1;ds_secret_free(stored);ds_secret_free(text);if(rc)return -1;
    // 安全存储读回成功后才移除明文；新换入的文件不能误删。
    // Remove plaintext only after secure-store readback; never remove a replaced file.
    if(lstat(file,&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||unlink(file))return 1;
    return 0;
}
int dm_r2_connect(const char *text){
    char *preview=dm_r2_connection_preview(text);if(!preview)return -1;free(preview);if(ds_credentials_store(text))return -1;char *stored=ds_credentials_load();int ok=stored&&!strcmp(stored,text);ds_secret_free(stored);if(!ok)return -1;
    status("connection_saved_restart",NULL,NULL);return 0;
}
int dm_r2_disconnect(void){
    // 安全存储删除失败不能报告已断开；保留连接状态以便明确重试。
    // Do not report disconnection if vault deletion fails; retain connection state for an explicit retry.
    if(ds_credentials_store(NULL)){status("credential_error",NULL,NULL);return -1;}
    atomic_store(&enabled,0);atomic_store(&cancelled,1);ds_transport_cancel();status("disconnected",NULL,NULL);return 0;
}
const char *dm_r2_message(const char *s){
    const char *pairs[][2]={
        {"credential_error","dmSyncConnectionError"},{"confirmation_changed","dmSyncConfirmationChanged"},{"disconnected","dmSyncDisconnected"},{"checking","dmSyncChecking"},
        {"local_saved","dmSyncLocalSaved"},{"synced","dmSyncSynced"},{"empty","dmSyncEmpty"},
        {"uploading","dmSyncUploading"},{"downloading","dmSyncDownloading"},
        {"offline","dmSyncOffline"},{"pending_upload","dmSyncPendingUpload"},
        {"pending_download","dmSyncPendingDownload"},{"restart_required","dmSyncRestartRequired"},
        {"connection_saved_restart","dmSyncConnectionSaved"},{"connection_file_ready","dmSyncFileReady"},
        {"conflict","dmSyncConflict"},
        {"confirm_empty","dmSyncConfirmEmpty"},
        {"unauthorized","dmSyncUnauthorized"},
        {"incompatible","dmSyncIncompatible"},{"invalid_download","dmSyncInvalidDownload"},
        {"invalid_local","dmSyncInvalidLocal"},{"disk_error","dmSyncDiskError"},
        {"snapshot_failed","dmSyncSnapshotFailed"},{"worker_failed","dmSyncWorkerFailed"},
        {"version_missing","dmSyncVersionMissing"},{"invalid_connection","dmSyncInvalidConnection"}};
    for(size_t i=0;i<sizeof pairs/sizeof *pairs;i++)if(same(s,pairs[i][0]))return dm_ui(pairs[i][1]);
    return dm_ui("dmSyncTitle");
}
int dm_r2_boot(void){
    if(atomic_exchange(&booted,1))return atomic_load(&enabled);
    char *text=NULL;
#ifdef DM_SYNC_TESTING
    if(getenv("DM_SYNC_TEST")&&getenv("DM_SYNC_CONNECTION")){size_t n;text=(char *)ds_read(getenv("DM_SYNC_CONNECTION"),&n);}
    else
#endif
    text=ds_credentials_load();
    if(!text){char *file=dm_r2_connection_file();if(file){free(file);status("connection_file_ready",NULL,NULL);return 1;}return 0;}cJSON *cfg=cJSON_Parse(text);free(text);
    if(!cfg||!valid_config(cfg)){cJSON_Delete(cfg);status("invalid_connection",NULL,NULL);return 1;}
    const char *sv=getenv("DM_SAVE");int test=getenv("DM_SAVE_TEST")||getenv("DM_SAVE_FRESH")||getenv("DM_APP_SUPPORT");
    if(!sv||strlen(sv)>1450||(test&&!(getenv("DM_SYNC_TEST")&&cJSON_IsTrue(item(cfg,"test"))))||
       (cJSON_IsTrue(item(cfg,"test"))&&!test)){cJSON_Delete(cfg);return 1;}
    const char *last=strrchr(sv,'/');if(!last||(strcmp(last+1,"save")&&!(test&&!strcmp(last+1,"save.test")))){cJSON_Delete(cfg);return 1;}
#ifdef DM_SYNC_TESTING
    /* Exercise the actual platform vault with isolated test credentials. The
     * plaintext handoff is removed only after a verified secure-store read. */
    if(getenv("DM_SYNC_TEST")&&getenv("DM_SYNC_IMPORT")&&getenv("DM_SYNC_CONNECTION")&&cJSON_IsTrue(item(cfg,"test"))){
        char *json=cJSON_PrintUnformatted(cfg),*stored=NULL;int rc=!json||ds_credentials_store(json);
        if(!rc){stored=ds_credentials_load();rc=!stored||strcmp(json,stored);}
        free(json);free(stored);if(rc){cJSON_Delete(cfg);status("invalid_connection",NULL,NULL);return 1;}
        unlink(getenv("DM_SYNC_CONNECTION"));
    }
#endif
    /* Apple exposes /var/mobile and /var/folders aliases. Canonicalize the trusted
     * app directory once; archive extraction still rejects all symlinks below it. */
    char canonical[PATH_MAX];
    if(!realpath(sv,canonical)){
        char parent[1536];snprintf(parent,sizeof parent,"%s",sv);*strrchr(parent,'/')=0;
        if(!realpath(parent,canonical)||strlen(canonical)+strlen(last)+1>=sizeof canonical){cJSON_Delete(cfg);status("disk_error",NULL,NULL);return 1;}
        strcat(canonical,last);
    }
    if(strlen(canonical)>=sizeof savepath){cJSON_Delete(cfg);return 1;}
    snprintf(savepath,sizeof savepath,"%s",canonical);snprintf(endpoint,sizeof endpoint,"%s",str(cfg,"url"));
    snprintf(token,sizeof token,"%s",str(cfg,"token"));snprintf(device,sizeof device,"%s",str(cfg,"device"));
    char namespace[65];ds_hash(endpoint,strlen(endpoint),namespace);snprintf(root,sizeof root,"%s.r2/%.16s",savepath,namespace);
    cJSON_Delete(cfg);if(ds_mkdirs(root)){status("disk_error",NULL,NULL);return 1;}
    state=readjson("state.json");char statefile[2048];path(statefile,"state.json");
    if((!state&&!access(statefile,F_OK))||(state&&!cJSON_IsObject(state))){cJSON_Delete(state);state=NULL;status("disk_error",NULL,NULL);return 1;}
    if(!state)state=cJSON_CreateObject();
    atomic_store(&enabled,1);status("checking",NULL,NULL);json_timeout_ms=5000;sync_run(1);json_timeout_ms=30000;
    extern void (*g_dm_save_commit_hook)(const char *);extern void (*g_dm_ingame_hook)(int);extern void (*g_dm_charsel_hook)(void);
    g_dm_save_commit_hook=dm_r2_on_commit;g_dm_ingame_hook=ingame;g_dm_charsel_hook=charsel;
    /* Capture before the guest mounts/creates its working directory. */
    char cap[2048];path(cap,"capture");ds_remove(cap);
    if(access(savepath,F_OK))ds_mkdirs(savepath);if(clonefile(savepath,cap,0))status("snapshot_failed",NULL,NULL);
    pthread_t th;if(pthread_create(&th,NULL,worker,NULL)){atomic_store(&enabled,0);status("worker_failed",NULL,NULL);}else{
        pthread_detach(th);cJSON *pending=readjson("pending.json");if(pending){cJSON_Delete(pending);dm_r2_command("sync","");}
#ifdef DM_SYNC_TESTING
        if(getenv("DM_SYNC_TEST")&&getenv("DM_SYNC_ACTION"))dm_r2_command(getenv("DM_SYNC_ACTION"),getenv("DM_SYNC_REVISION"));
#endif
    }
    return 1;
}
