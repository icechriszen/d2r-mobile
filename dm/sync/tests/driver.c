/* A separate process per simulated app launch exercises the real disk journal. */
#include "../dm_sync.h"
#include "../dm_sync_ui.h"
const char *dm_ui(const char *key){return key;}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
void (*g_dm_save_commit_hook)(const char *);
void (*g_dm_ingame_hook)(int);
void (*g_dm_charsel_hook)(void);
char *ds_credentials_load(void){const char *p=getenv("DM_TEST_CREDENTIAL_STORE")?:getenv("DM_SYNC_CONNECTION");size_t n;return p?(char *)ds_read(p,&n):NULL;}
int ds_credentials_store(const char *s){const char *p=getenv("DM_TEST_CREDENTIAL_STORE");if(!p||getenv("DM_TEST_STORE_FAIL"))return -1;return s?ds_write(p,s,strlen(s)):(unlink(p)==0||errno==ENOENT?0:-1);}
void ds_platform_paste(void (*complete)(char *)){const char *file=getenv("DM_TEST_PASTE_FILE");size_t n;complete(file?(char*)ds_read(file,&n):NULL);}
static cJSON *ui_view(void){
    const ds_ui_view *v=dm_sync_ui_view();cJSON *o=cJSON_CreateObject(),*a=cJSON_AddArrayToObject(o,"buttons");
    cJSON_AddNumberToObject(o,"page",v->page);cJSON_AddNumberToObject(o,"selection",v->selection);
    cJSON_AddStringToObject(o,"left",v->left);cJSON_AddStringToObject(o,"right",v->right);cJSON_AddStringToObject(o,"status",v->status);
    for(int i=0;i<v->count;i++){cJSON *b=cJSON_CreateObject();cJSON_AddStringToObject(b,"action",v->buttons[i].action);cJSON_AddStringToObject(b,"label",v->buttons[i].label);cJSON_AddBoolToObject(b,"enabled",v->buttons[i].enabled);cJSON_AddItemToArray(a,b);}return o;
}
int main(int argc,char **argv){
    if(argc<3)return 64;
    if(!strcmp(argv[1],"snapshot")){
        if(argc!=4)return 64;unsigned char *zip=NULL;size_t n=0;cJSON *m=NULL;
        if(ds_snapshot(argv[2],"0123456789abcdef0123456789abcdef",NULL,&zip,&n,&m))return 2;
        int rc=ds_write(argv[3],zip,n);char *s=cJSON_PrintUnformatted(m);puts(s);free(s);free(zip);cJSON_Delete(m);return rc?3:0;
    }
    if(!strcmp(argv[1],"apply")){
        if(argc!=5)return 64;size_t n;unsigned char *z=ds_read(argv[2],&n);if(!z)return 2;
        int rc=ds_install(z,n,argv[3],argv[4]);free(z);return rc?3:0;
    }
    setenv("DM_SAVE",argv[2],1);setenv("DM_SYNC_TEST","1",1);setenv("DM_SAVE_TEST","1",1);
    if(!strcmp(argv[1],"import")){
        if(argc>3&&!strcmp(argv[3],"accept")){printf("%d\n",dm_r2_import_connection());return 0;}
        char *public=dm_r2_connection_file();puts(public?:"null");free(public);return 0;
    }
    if(!dm_r2_boot())return 4;
    if(!dm_r2_wait(90))return 5;
    if(argc>4&&!strcmp(argv[3],"ui")){
        char *script=strdup(argv[4]),*next=NULL;cJSON *views=cJSON_CreateArray();
        for(char *step=strtok_r(script,",",&next);step;step=strtok_r(NULL,",",&next)){
            if(!strcmp(step,"open"))dm_sync_ui_open();else if(!strcmp(step,"wait"))dm_r2_wait(90);
            else if(!strcmp(step,"down"))dm_sync_ui_select(1);else if(!strcmp(step,"up"))dm_sync_ui_select(-1);
            else if(strcmp(step,"tick"))dm_sync_ui_action(step);
            dm_sync_ui_tick();cJSON_AddItemToArray(views,ui_view());
        }
        free(script);char *out=cJSON_PrintUnformatted(views);puts(out);free(out);cJSON_Delete(views);return 0;
    }
    if(argc>4&&!strncmp(argv[3],"checked-",8)){
        cJSON *expect=cJSON_Parse(argv[4]);cJSON *h=cJSON_GetObjectItemCaseSensitive(expect,"head"),*hash=cJSON_GetObjectItemCaseSensitive(expect,"hash");
        if(!dm_r2_submit(argv[3]+8,"",cJSON_IsString(h)?h->valuestring:"",cJSON_IsString(hash)?hash->valuestring:""))return 7;
        cJSON_Delete(expect);if(!dm_r2_wait(90))return 5;char *out=dm_r2_status();puts(out);free(out);return 0;
    }
    if(argc>3){
        if(!strcmp(argv[3],"commit")&&argc>4){
            size_t n;unsigned char *bytes=ds_read(argv[4],&n);char file[2048];snprintf(file,sizeof file,"%s/hero/hero.d2s",argv[2]);
            if(!bytes||ds_write(file,bytes,n)){free(bytes);return 6;}free(bytes);
            g_dm_ingame_hook(1);g_dm_save_commit_hook(argv[2]);
        }else dm_r2_command(argv[3],argc>4?argv[4]:"");
        if(!dm_r2_wait(90))return 5;
    }
    char *s=dm_r2_status();puts(s);free(s);return 0;
}
