#include "dm_sync.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define DS_STRING_(x) #x
#define DS_STRING(x) DS_STRING_(x)
#ifndef DS_SOURCE_COMMIT
#define DS_SOURCE_COMMIT unknown
#endif

typedef struct { char *name, *key; unsigned char *data; size_t size; uint32_t crc, offset; } Entry;
typedef struct { Entry *items; size_t count, total; double savedAt; } Entries;
static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1]<<8); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)u16(p) | (uint32_t)u16(p+2)<<16; }
static void w16(unsigned char *p, uint16_t x) { p[0]=x; p[1]=x>>8; }
static void w32(unsigned char *p, uint32_t x) { w16(p,x); w16(p+2,x>>16); }
static uint32_t crc32bytes(const void *data, size_t n) {
    uint32_t c=~0u; const unsigned char *p=data;
    while(n--) { c^=*p++; for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & (0u-(c&1))); } return ~c;
}
void ds_hash(const void *data,size_t n,char out[65]) {
    unsigned char h[32]; CC_SHA256(data,(CC_LONG)n,h);
    for(int i=0;i<32;i++) sprintf(out+2*i,"%02x",h[i]);
}
void ds_uuid(char out[33]) { unsigned char b[16]; arc4random_buf(b,sizeof b); for(int i=0;i<16;i++) sprintf(out+i*2,"%02x",b[i]); }
int ds_mkdirs(const char *p) {
    char s[2048]; if(strlen(p)>=sizeof s) return -1; strcpy(s,p);
    for(char *q=s+1;;q++) if(*q=='/'||!*q) { char c=*q; *q=0;
        if(mkdir(s,0700)&&errno!=EEXIST) return -1;
        struct stat st; if(lstat(s,&st)||!S_ISDIR(st.st_mode)) return -1; *q=c; if(!c) break; }
    return 0;
}
static void sync_parent(const char *path) {
    char p[2048]; snprintf(p,sizeof p,"%s",path); char *q=strrchr(p,'/'); if(!q)return; *q=0;
    int fd=open(p,O_RDONLY); if(fd>=0){fsync(fd);close(fd);}
}
int ds_write(const char *path,const void *data,size_t n) {
    char tmp[2100],id[33]; ds_uuid(id); if(snprintf(tmp,sizeof tmp,"%s.%s.tmp",path,id)>=(int)sizeof tmp)return -1;
    int fd=open(tmp,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600), ok=0; if(fd<0)return -1;
    const unsigned char *p=data; size_t left=n;
    while(left){ssize_t k=write(fd,p,left);if(k<0&&errno==EINTR)continue;if(k<=0)goto done;p+=k;left-=k;}
    if(fsync(fd))goto done; if(close(fd)){fd=-1;goto done;} fd=-1;
    if(rename(tmp,path))goto done;sync_parent(path);ok=1;
done: if(fd>=0)close(fd);if(!ok)unlink(tmp);return ok?0:-1;
}
unsigned char *ds_read(const char *p,size_t *n) {
    *n=0; int fd=open(p,O_RDONLY|O_NOFOLLOW);if(fd<0)return NULL; struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>DS_LIMIT){close(fd);return NULL;}
    size_t cap=(size_t)st.st_size;unsigned char *b=malloc(cap+1);if(!b){close(fd);return NULL;}
    while(*n<cap){ssize_t k=read(fd,b+*n,cap-*n);if(k<0&&errno==EINTR)continue;if(k<=0){free(b);close(fd);return NULL;}*n+=(size_t)k;}
    unsigned char extra; if(read(fd,&extra,1)!=0){free(b);close(fd);return NULL;}
    close(fd);b[cap]=0;return b;
}
int ds_remove(const char *p) {
    struct stat st;if(lstat(p,&st))return errno==ENOENT?0:-1;
    if(!S_ISDIR(st.st_mode))return unlink(p);
    DIR *d=opendir(p);if(!d)return -1;struct dirent *e;int rc=0;char q[2048];
    while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(snprintf(q,sizeof q,"%s/%s",p,e->d_name)>=(int)sizeof q||ds_remove(q))rc=-1;}
    closedir(d);return rc?rc:rmdir(p);
}
static int suffix(const char *s,const char *end){size_t a=strlen(s),b=strlen(end);return a>=b&&!strcmp(s+a-b,end);}
int ds_path_allowed(const char *p) {
    if(!p||!*p||strlen(p)>240||strchr(p,'\\')||strchr(p,':'))return 0;
    for(const unsigned char *q=(const unsigned char *)p;*q;q++)if(*q<32||*q==127)return 0;
    char s[256];strcpy(s,p);char *slash=strchr(s,'/');
    if(!slash)return (!strncmp(s,"SharedStash",11)||!strncmp(s,"ModernSharedStash",17))&&(suffix(s,".d2i")||suffix(s,".swvault"));
    *slash++=0;if(!*s||!slash[0]||strchr(slash,'/')||s[0]=='.'||s[0]=='_'||slash[0]=='.')return 0;
    if(!strcasecmp(s,"Settings")||!strcasecmp(s,"temp")||!strcasecmp(s,"logs")||!strcasecmp(s,"cache")||!strncasecmp(s,"save.",5))return 0;
    return 1;
}
/* Headless tools without a platform normalizer fail closed on non-ASCII names. */
__attribute__((weak)) char *ds_path_key(const char *p){char *key=strdup(p);if(!key)return NULL;for(unsigned char *q=(unsigned char *)key;*q;q++){if(*q>=128){free(key);return NULL;}if(*q>='A'&&*q<='Z')*q+=32;}return key;}
static void clear(Entries *es){for(size_t i=0;i<es->count;i++){free(es->items[i].name);free(es->items[i].key);free(es->items[i].data);}free(es->items);memset(es,0,sizeof *es);}
static int append(Entries *es,const char *name,unsigned char *data,size_t n) {
    if(es->count>=2049||es->total+n>DS_LIMIT){free(data);return -1;}
    char *key=ds_path_key(name);if(!key){free(data);return -1;}
    for(size_t i=0;i<es->count;i++)if(!strcmp(es->items[i].key,key)){free(key);free(data);return -1;}
    Entry *items=realloc(es->items,(es->count+1)*sizeof *items);if(!items){free(key);free(data);return -1;}
    es->items=items;Entry *x=&items[es->count++];*x=(Entry){strdup(name),key,data,n,crc32bytes(data,n),0};es->total+=n;return x->name?0:-1;
}
static int cmp(const void *a,const void *b){return strcmp(((const Entry *)a)->name,((const Entry *)b)->name);}
static int sidecar(const char *path,const unsigned char *b,size_t n){
    if(suffix(path,".d2i")){
        size_t at=0;if(!n)return -1;
        while(at<n){if(n-at<68||u32(b+at)!=0xaa55aa55||u32(b+at+8)!=105)return -1;
            size_t len=u32(b+at+16);if(len<68||len>n-at)return -1;
            if(memcmp(b+at+64,"JM",2)&&(len<76||u32(b+at+64)!=0xc0eaedc0||u16(b+at+68)!=1))return -1;
            at+=len;}return 0;
    }
    int vault=suffix(path,".swvault"),pouch=suffix(path,".swpouch");
    if(vault||pouch){
        if(n<8||memcmp(b,vault?"SWVT":"SWPU",4)||(vault?u16(b+4)!=1:b[4]!=1))return -1;
        size_t at=8;unsigned count=u16(b+6);
        for(unsigned i=0;i<count;i++){if(n-at<6)return -1;size_t len=u16(b+at+4);at+=6;if(!len||len>n-at)return -1;at+=len;}
        return at==n?0:-1;
    }
    if(suffix(path,".swpage")&&(n<8||memcmp(b,"SWPG",4)||(b[4]!=1&&b[4]!=2)))return -1;
    return 0;
}
static int hero(const char *path,const unsigned char *b,size_t n,cJSON *heroes) {
    if(!suffix(path,".d2s"))return 0;
    uint32_t sum=0;for(size_t i=0;i<n;i++)sum=(sum<<1|sum>>31)+(i>=12&&i<16?0:b[i]);
    if(n<0x15b||u32(b)!=0xaa55aa55||u32(b+4)!=105||u32(b+8)!=n||u32(b+12)!=sum||b[0x18]>7||b[0x1b]<1||b[0x1b]>99)return -1;
    const char *slash=strchr(path,'/');if(!slash)return -1;char name[128];size_t len=(size_t)(slash-path);if(len>=sizeof name)return -1;
    memcpy(name,path,len);name[len]=0;char expected[256];snprintf(expected,sizeof expected,"%s/%s.d2s",name,name);if(strcmp(path,expected))return -1;
    cJSON *h=cJSON_CreateObject();cJSON_AddStringToObject(h,"name",name);cJSON_AddNumberToObject(h,"level",b[0x1b]);cJSON_AddNumberToObject(h,"classId",b[0x18]);cJSON_AddItemToArray(heroes,h);return 0;
}
static int zip_write(Entries *es,unsigned char **out,size_t *size) {
    size_t n=22;for(size_t i=0;i<es->count;i++)n+=76+2*strlen(es->items[i].name)+es->items[i].size;
    if(n>DS_LIMIT)return -1;unsigned char *b=calloc(1,n);if(!b)return -1;size_t at=0;
    for(size_t i=0;i<es->count;i++){Entry *e=&es->items[i];size_t l=strlen(e->name);e->offset=(uint32_t)at;
        w32(b+at,0x04034b50);w16(b+at+4,20);w16(b+at+6,0x800);w16(b+at+12,33);
        w32(b+at+14,e->crc);w32(b+at+18,(uint32_t)e->size);w32(b+at+22,(uint32_t)e->size);w16(b+at+26,(uint16_t)l);
        memcpy(b+at+30,e->name,l);memcpy(b+at+30+l,e->data,e->size);at+=30+l+e->size;}
    size_t cd=at;
    for(size_t i=0;i<es->count;i++){Entry *e=&es->items[i];size_t l=strlen(e->name);
        w32(b+at,0x02014b50);w16(b+at+4,20);w16(b+at+6,20);w16(b+at+8,0x800);w16(b+at+14,33);
        w32(b+at+16,e->crc);w32(b+at+20,(uint32_t)e->size);w32(b+at+24,(uint32_t)e->size);w16(b+at+28,(uint16_t)l);w32(b+at+42,e->offset);
        memcpy(b+at+46,e->name,l);at+=46+l;}
    w32(b+at,0x06054b50);w16(b+at+8,(uint16_t)es->count);w16(b+at+10,(uint16_t)es->count);w32(b+at+12,(uint32_t)(at-cd));w32(b+at+16,(uint32_t)cd);
    *out=b;*size=n;return 0;
}
static int zip_read(const unsigned char *b,size_t n,Entries *es) {
    if(n<22||n>DS_LIMIT||u32(b+n-22)!=0x06054b50||u16(b+n-18)||u16(b+n-16)||u16(b+n-2))return -1;
    uint16_t count=u16(b+n-12);size_t cd=u32(b+n-6),at=cd;
    if(count>2049||count!=u16(b+n-14)||cd+u32(b+n-10)!=n-22)return -1;
    size_t next=0;
    for(unsigned i=0;i<count;i++){
        if(at>n-22||n-22-at<46||u32(b+at)!=0x02014b50)return -1;
        size_t l=u16(b+at+28),sz=u32(b+at+24),off=u32(b+at+42);
        if(!l||l>255||u16(b+at+10)||u16(b+at+30)||u16(b+at+32)||u16(b+at+34)||
           (u16(b+at+8)&~0x800)||u32(b+at+20)!=sz||at+46+l>n-22||off!=next||off+30>cd)return -1;
        if(u32(b+off)!=0x04034b50||u16(b+off+8)||u16(b+off+28)||u16(b+off+26)!=l||
           u16(b+off+6)!=u16(b+at+8)||u32(b+off+14)!=u32(b+at+16)||
           u32(b+off+18)!=sz||u32(b+off+22)!=sz||off+30+l+sz>cd||memcmp(b+off+30,b+at+46,l))return -1;
        char name[256];memcpy(name,b+at+46,l);name[l]=0;if(strlen(name)!=l)return -1;
        if(strcmp(name,"manifest.json")&&(strncmp(name,"save/",5)||!ds_path_allowed(name+5)))return -1;
        unsigned char *data=malloc(sz+1);if(!data)return -1;memcpy(data,b+off+30+l,sz);data[sz]=0;
        if(crc32bytes(data,sz)!=u32(b+at+16)){free(data);return -1;}
        if(append(es,name,data,sz))return -1;next=off+30+l+sz;at+=46+l;
    }
    return at==n-22&&next==cd?0:-1;
}
static cJSON *manifest_for(Entries *es,const char *device) {
    qsort(es->items,es->count,sizeof(Entry),cmp);
    cJSON *m=cJSON_CreateObject(),*files=cJSON_AddArrayToObject(m,"files"),*heroes=cJSON_AddArrayToObject(m,"characters");
    cJSON_AddNumberToObject(m,"protocol",1);cJSON_AddNumberToObject(m,"format",1);cJSON_AddStringToObject(m,"game",DS_GAME);
    cJSON_AddStringToObject(m,"device",device);cJSON_AddStringToObject(m,"source",DS_STRING(DS_SOURCE_COMMIT));cJSON_AddNumberToObject(m,"savedAt",es->savedAt?es->savedAt:(double)time(NULL)*1000);
    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);unsigned char digest[32];char content[65];
    for(size_t i=0;i<es->count;i++){Entry *e=&es->items[i];const char *p=e->name+5;char hash[65];ds_hash(e->data,e->size,hash);
        if(hero(p,e->data,e->size,heroes)||sidecar(p,e->data,e->size)){cJSON_Delete(m);return NULL;}
        cJSON *f=cJSON_CreateObject();cJSON_AddStringToObject(f,"path",p);cJSON_AddNumberToObject(f,"size",(double)e->size);cJSON_AddStringToObject(f,"sha256",hash);cJSON_AddItemToArray(files,f);
        CC_SHA256_Update(&ctx,p,(CC_LONG)strlen(p)+1);CC_SHA256_Update(&ctx,hash,64);CC_SHA256_Update(&ctx,"\n",1);}
    CC_SHA256_Final(digest,&ctx);for(int i=0;i<32;i++)sprintf(content+2*i,"%02x",digest[i]);cJSON_AddStringToObject(m,"contentHash",content);return m;
}
static int scan(const char *root,Entries *es) {
    DIR *d=opendir(root);if(!d)return errno==ENOENT?0:-1;struct dirent *e;int rc=0;
    while(!rc&&(e=readdir(d))){if(e->d_name[0]=='.'||e->d_name[0]=='_'||!strcasecmp(e->d_name,"Settings"))continue;
        char p[2048];snprintf(p,sizeof p,"%s/%s",root,e->d_name);struct stat st;if(lstat(p,&st)){rc=-1;break;}
        if(S_ISDIR(st.st_mode)){
            char check[2048];snprintf(check,sizeof check,"%s/%s.d2s",p,e->d_name);
            if(strncmp(e->d_name,"ModernSharedStash",17)&&access(check,F_OK))continue;
            DIR *sub=opendir(p);if(!sub){rc=-1;break;}struct dirent *f;
            while(!rc&&(f=readdir(sub))){if(f->d_name[0]=='.'||suffix(f->d_name,".tmp"))continue;
                char path[2048],name[256];snprintf(path,sizeof path,"%s/%s",p,f->d_name);
                if(snprintf(name,sizeof name,"save/%s/%s",e->d_name,f->d_name)>=(int)sizeof name||!ds_path_allowed(name+5)){rc=-1;break;}
                size_t n;unsigned char *bytes=ds_read(path,&n);if(!bytes||append(es,name,bytes,n))rc=-1;
                if(!rc){struct stat saved;if(lstat(path,&saved))rc=-1;else if((double)saved.st_mtime*1000>es->savedAt)es->savedAt=(double)saved.st_mtime*1000;}}
            closedir(sub);
        }else if(ds_path_allowed(e->d_name)){
            char name[256];snprintf(name,sizeof name,"save/%s",e->d_name);size_t n;unsigned char *b=ds_read(p,&n);
            if(!b||append(es,name,b,n))rc=-1;
            if(!rc&&(double)st.st_mtime*1000>es->savedAt)es->savedAt=(double)st.st_mtime*1000;
        }else if(!S_ISREG(st.st_mode)){rc=-1;}
    }closedir(d);return rc;
}
int ds_snapshot(const char *root,const char *device,const char *parent,unsigned char **zip,size_t *n,cJSON **manifest) {
    Entries es={0};int rc=-1;*zip=NULL;*n=0;*manifest=NULL;
    if(scan(root,&es))goto done;
    *manifest=manifest_for(&es,device);if(!*manifest)goto done;
    cJSON_AddItemToObject(*manifest,"parent",parent&&*parent?cJSON_CreateString(parent):cJSON_CreateNull());
    char *json=cJSON_PrintUnformatted(*manifest);if(!json)goto done;
    if(append(&es,"manifest.json",(unsigned char *)json,strlen(json)))goto done;
    rc=zip_write(&es,zip,n);
done:clear(&es);if(rc){cJSON_Delete(*manifest);*manifest=NULL;}return rc;
}
int ds_verify(const unsigned char *zip,size_t n,cJSON **manifest) {
    Entries es={0};int rc=-1;*manifest=NULL;if(zip_read(zip,n,&es))goto done;
    size_t mi=es.count;for(size_t i=0;i<es.count;i++)if(!strcmp(es.items[i].name,"manifest.json"))mi=i;
    if(mi==es.count)goto done;
    cJSON *m=cJSON_ParseWithLength((char *)es.items[mi].data,es.items[mi].size);if(!m)goto done;
    cJSON *p=cJSON_GetObjectItemCaseSensitive(m,"protocol"),*f=cJSON_GetObjectItemCaseSensitive(m,"format"),*g=cJSON_GetObjectItemCaseSensitive(m,"game"),*dv=cJSON_GetObjectItemCaseSensitive(m,"device");
    cJSON *parent=cJSON_GetObjectItemCaseSensitive(m,"parent"),*saved=cJSON_GetObjectItemCaseSensitive(m,"savedAt");
    if(!cJSON_IsNumber(p)||p->valuedouble!=1||!cJSON_IsNumber(f)||f->valuedouble!=1||!cJSON_IsString(g)||strcmp(g->valuestring,DS_GAME)||!cJSON_IsString(dv)||
       !cJSON_IsNumber(saved)||saved->valuedouble<0||saved->valuedouble>253402300799999.0||
       !(cJSON_IsNull(parent)||(cJSON_IsString(parent)&&strlen(parent->valuestring)==32))){cJSON_Delete(m);goto done;}
    free(es.items[mi].name);free(es.items[mi].key);free(es.items[mi].data);es.items[mi]=es.items[--es.count];
    cJSON *actual=manifest_for(&es,dv->valuestring);
    if(actual){const char *keys[]={"files","characters","contentHash"};
        rc=0;for(int i=0;i<3;i++)if(!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(actual,keys[i]),cJSON_GetObjectItemCaseSensitive(m,keys[i]),1))rc=-1;
        cJSON_Delete(actual);}
    if(rc)cJSON_Delete(m);else *manifest=m;
done:clear(&es);return rc;
}
static void trim_backups(const char *root) {
    struct dirent **list=NULL;int n=scandir(root,&list,NULL,alphasort),count=0;
    if(n<0)return;
    for(int i=n-1;i>=0;i--){if(list[i]->d_name[0]!='.'){if(++count>5){char p[2048];snprintf(p,sizeof p,"%s/%s",root,list[i]->d_name);ds_remove(p);}}free(list[i]);}free(list);
}
static unsigned long long next_backup(const char *root){
    unsigned long long latest=0;DIR *d=opendir(root);if(!d)return 1;struct dirent *e;
    while((e=readdir(d))){char *end;unsigned long long n=strtoull(e->d_name,&end,10);if(end!=e->d_name&&*end=='-'&&n>latest)latest=n;}
    closedir(d);return latest+1;
}
static int cleanup_stages(const char *save){
    char parent[2048],prefix[256];snprintf(parent,sizeof parent,"%s",save);char *base=strrchr(parent,'/');if(!base)return -1;
    if(snprintf(prefix,sizeof prefix,"%s.r2-new-",base+1)>=(int)sizeof prefix)return -1;*base=0;
    DIR *d=opendir(parent);if(!d)return -1;struct dirent *e;int rc=0;size_t n=strlen(prefix);
    while((e=readdir(d))){if(strncmp(e->d_name,prefix,n)||strlen(e->d_name)!=n+32)continue;
        const char *id=e->d_name+n;if(strspn(id,"0123456789abcdef")!=32)continue;
        char path[2048];if(snprintf(path,sizeof path,"%s/%s",parent,e->d_name)>=(int)sizeof path||ds_remove(path)){rc=-1;break;}
    }closedir(d);return rc;
}
int ds_install(const unsigned char *zip,size_t n,const char *save,const char *backups) {
    cJSON *m=NULL;if(ds_verify(zip,n,&m))return -1;
    double savedAt=cJSON_GetObjectItemCaseSensitive(m,"savedAt")->valuedouble;cJSON_Delete(m);
    struct timespec savedTimes[2]={{(time_t)(savedAt/1000),0},{(time_t)(savedAt/1000),(long)((uint64_t)savedAt%1000)*1000000}};
    Entries es={0};if(zip_read(zip,n,&es)){clear(&es);return -1;}
    char stage[2048],backup[2048],id[33];ds_uuid(id);
    snprintf(stage,sizeof stage,"%s.r2-new-%s",save,id);snprintf(backup,sizeof backup,"%s/%020llu-%s",backups,next_backup(backups),id);
    int rc=-1;struct stat st;int exists=!lstat(save,&st);
    if(exists&&!S_ISDIR(st.st_mode))goto done;
    if(cleanup_stages(save))goto done;
    if(ds_mkdirs(backups))goto done;
    if(exists){if(clonefile(save,backup,0)||clonefile(save,stage,0))goto done;}else if(mkdir(stage,0700))goto done;
    /* Keep settings and non-game directories from this device; remove old game units. */
    Entries old={0};if(scan(stage,&old)){clear(&old);goto done;}
    for(size_t i=0;i<old.count;i++){char p[2048];snprintf(p,sizeof p,"%s/%s",stage,old.items[i].name+5);if(unlink(p)){clear(&old);goto done;}}
    clear(&old);
    for(size_t i=0;i<es.count;i++){Entry *e=&es.items[i];if(!strcmp(e->name,"manifest.json"))continue;
        char p[2048],dir[2048];snprintf(p,sizeof p,"%s/%s",stage,e->name+5);strcpy(dir,p);char *s=strrchr(dir,'/');*s=0;
        if(ds_mkdirs(dir)||ds_write(p,e->data,e->size)||utimensat(AT_FDCWD,p,savedTimes,AT_SYMLINK_NOFOLLOW))goto done;}
#ifdef DM_SYNC_TEST_DRIVER
    if(getenv("DM_SYNC_CRASH_BEFORE_SWAP"))_exit(85);
#endif
    if(exists?renamex_np(stage,save,RENAME_SWAP):rename(stage,save))goto done;
    sync_parent(save);rc=0;trim_backups(backups);
done:ds_remove(stage);clear(&es);return rc;
}
