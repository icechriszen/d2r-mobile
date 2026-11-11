#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../sync/dm_sync.h"
#include "../sync/dm_sync_ui.h"
#include "../host/dm_i18n.h"
static JavaVM *sync_vm;
static jclass sync_class;
static JNIEnv *env_get(int *detach){
    JNIEnv *env=NULL;*detach=0;if(!sync_vm)return NULL;
    if((*sync_vm)->GetEnv(sync_vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK){
        if((*sync_vm)->AttachCurrentThread(sync_vm,&env,NULL)!=JNI_OK)return NULL;*detach=1;
    }return env;
}
/* JNI NewStringUTF/GetStringUTFChars use modified UTF-8. Cloud JSON and
 * filenames use standard UTF-8, including supplementary Unicode characters. */
static jstring java_utf8(JNIEnv *e,const char *text){
    if(!text||(*e)->ExceptionCheck(e))return NULL;
    size_t n=strlen(text);if(n>DS_LIMIT)return NULL;
    jclass cls=(*e)->FindClass(e,"java/lang/String");if(!cls)return NULL;
    jstring encoding=NULL,out=NULL;jbyteArray bytes=NULL;
    jmethodID init=(*e)->GetMethodID(e,cls,"<init>","([BLjava/lang/String;)V");
    if(!init)goto done;
    bytes=(*e)->NewByteArray(e,(jsize)n);if(!bytes)goto done;
    encoding=(*e)->NewStringUTF(e,"UTF-8");if(!encoding)goto done;
    (*e)->SetByteArrayRegion(e,bytes,0,(jsize)n,(const jbyte *)text);
    if(!(*e)->ExceptionCheck(e))out=(*e)->NewObject(e,cls,init,bytes,encoding);
done:
    if(bytes)(*e)->DeleteLocalRef(e,bytes);if(encoding)(*e)->DeleteLocalRef(e,encoding);
    (*e)->DeleteLocalRef(e,cls);return out;
}
static char *native_utf8(JNIEnv *e,jstring text){
    if(!text||(*e)->ExceptionCheck(e))return NULL;
    jclass cls=(*e)->FindClass(e,"java/lang/String");if(!cls)return NULL;
    jstring encoding=NULL;jbyteArray bytes=NULL;char *out=NULL;
    jmethodID get=(*e)->GetMethodID(e,cls,"getBytes","(Ljava/lang/String;)[B");
    if(!get)goto done;
    encoding=(*e)->NewStringUTF(e,"UTF-8");if(!encoding)goto done;
    bytes=(*e)->CallObjectMethod(e,text,get,encoding);
    if(bytes&&!(*e)->ExceptionCheck(e)){
        jsize n=(*e)->GetArrayLength(e,bytes);
        if(n>=0&&(size_t)n<=DS_LIMIT){
            out=malloc((size_t)n+1);
            if(out){
                (*e)->GetByteArrayRegion(e,bytes,0,n,(jbyte *)out);
                if((*e)->ExceptionCheck(e)||memchr(out,0,(size_t)n)){free(out);out=NULL;}
                else out[n]=0;
            }
        }
    }
done:
    if(bytes)(*e)->DeleteLocalRef(e,bytes);if(encoding)(*e)->DeleteLocalRef(e,encoding);
    (*e)->DeleteLocalRef(e,cls);return out;
}
static void env_done(JNIEnv *env,int detach){if(env&&(*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(detach)(*sync_vm)->DetachCurrentThread(sync_vm);}
char *ds_path_key(const char *path){
    int detach;JNIEnv *e=env_get(&detach);if(!e)return NULL;char *out=NULL;
    jmethodID m=(*e)->GetStaticMethodID(e,sync_class,"pathKey","([B)[B");size_t n=strlen(path);
    jbyteArray input=(*e)->NewByteArray(e,(jsize)n);if(input)(*e)->SetByteArrayRegion(e,input,0,(jsize)n,(const jbyte *)path);
    jbyteArray result=m&&input?(*e)->CallStaticObjectMethod(e,sync_class,m,input):NULL;
    if(result&&!(*e)->ExceptionCheck(e)){jsize len=(*e)->GetArrayLength(e,result);out=malloc((size_t)len+1);
        if(out){(*e)->GetByteArrayRegion(e,result,0,len,(jbyte *)out);out[len]=0;}}
    if(input)(*e)->DeleteLocalRef(e,input);if(result)(*e)->DeleteLocalRef(e,result);env_done(e,detach);return out;
}
int dm_sync_android_init(JavaVM *vm,JNIEnv *env){
    sync_vm=vm;jclass cls=(*env)->FindClass(env,"com/xiaoboluo/d2r/CloudSaves");if(!cls)return -1;
    sync_class=(*env)->NewGlobalRef(env,cls);(*env)->DeleteLocalRef(env,cls);return sync_class?0:-1;
}
int ds_transport(const char *method,const char *url,const char *token,const void *body,size_t n,unsigned ms,unsigned char **out,size_t *size){
    *out=NULL;*size=0;if(n>DS_LIMIT)return -1;int detach;JNIEnv *e=env_get(&detach);if(!e)return -1;int rc=-1;
    jmethodID m=(*e)->GetStaticMethodID(e,sync_class,"request","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI)[B");
    jstring jm=java_utf8(e,method),ju=java_utf8(e,url),jt=java_utf8(e,token);jbyteArray jb=(*e)->NewByteArray(e,(jsize)n);
    if(jb&&n)(*e)->SetByteArrayRegion(e,jb,0,(jsize)n,body);
    jbyteArray result=m&&jm&&ju&&jt&&jb&&!(*e)->ExceptionCheck(e)?(*e)->CallStaticObjectMethod(e,sync_class,m,jm,ju,jt,jb,(jint)ms):NULL;
    if(result&&!(*e)->ExceptionCheck(e)){jsize len=(*e)->GetArrayLength(e,result);
        if(len>=4&&(size_t)len<=DS_LIMIT+4){unsigned char *buf=malloc((size_t)len+1);
            if(buf){(*e)->GetByteArrayRegion(e,result,0,len,(jbyte *)buf);rc=(int)((uint32_t)buf[0]<<24|(uint32_t)buf[1]<<16|(uint32_t)buf[2]<<8|buf[3]);
                memmove(buf,buf+4,(size_t)len-4);*size=(size_t)len-4;buf[*size]=0;*out=buf;}}}
    if(result)(*e)->DeleteLocalRef(e,result);(*e)->DeleteLocalRef(e,jm);(*e)->DeleteLocalRef(e,ju);(*e)->DeleteLocalRef(e,jt);(*e)->DeleteLocalRef(e,jb);env_done(e,detach);return rc;
}
char *ds_credentials_load(void){
    int d;JNIEnv *e=env_get(&d);if(!e)return NULL;jmethodID m=(*e)->GetStaticMethodID(e,sync_class,"credentialsLoad","()Ljava/lang/String;");
    jstring s=m?(*e)->CallStaticObjectMethod(e,sync_class,m):NULL;char *out=NULL;
    if(s&&!(*e)->ExceptionCheck(e)){out=native_utf8(e,s);(*e)->DeleteLocalRef(e,s);}
    env_done(e,d);return out;
}
int ds_credentials_store(const char *text){
    int d;JNIEnv *e=env_get(&d);if(!e)return -1;jmethodID m=(*e)->GetStaticMethodID(e,sync_class,"credentialsStore","(Ljava/lang/String;)Z");
    jstring s=java_utf8(e,text);int ok=m&&(s||!text)&&!(*e)->ExceptionCheck(e)&&(*e)->CallStaticBooleanMethod(e,sync_class,m,s)&&!(*e)->ExceptionCheck(e);
    if(s)(*e)->DeleteLocalRef(e,s);env_done(e,d);return ok?0:-1;
}
static int notify(const char *name){int d;JNIEnv *e=env_get(&d);if(!e)return 0;jmethodID m=(*e)->GetStaticMethodID(e,sync_class,name,"()V");if(m)(*e)->CallStaticVoidMethod(e,sync_class,m);int ok=m&&!(*e)->ExceptionCheck(e);env_done(e,d);return ok;}
void ds_platform_show(void){dm_sync_ui_open();}
void ds_platform_changed(void){dm_sync_ui_changed();notify("changed");}
void ds_transport_cancel(void){notify("cancelRequest");}
JNIEXPORT jstring JNICALL Java_com_xiaoboluo_d2r_CloudSaves_status(JNIEnv *e,jclass cls){
    (void)cls;char *s=dm_r2_status();cJSON *j=cJSON_Parse(s);free(s);cJSON *v=cJSON_GetObjectItemCaseSensitive(j,"status");
    cJSON_AddStringToObject(j,"message",dm_r2_message(cJSON_IsString(v)?v->valuestring:"disconnected"));s=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    jstring out=java_utf8(e,s?s:"{}");free(s);return out;
}
JNIEXPORT jstring JNICALL Java_com_xiaoboluo_d2r_CloudSaves_connectionFile(JNIEnv *e,jclass cls){(void)cls;char *s=dm_r2_connection_file();jstring out=s?java_utf8(e,s):NULL;free(s);return out;}
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_CloudSaves_importConnection(JNIEnv *e,jclass cls){(void)e;(void)cls;return dm_r2_import_connection();}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_CloudSaves_progress(JNIEnv *e,jclass cls,jlong received,jlong total){
    (void)e;(void)cls;if(received>=0&&total>=0)dm_r2_progress((size_t)received,(size_t)total);
}
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_CloudSaves_connect(JNIEnv *e,jclass cls,jstring code){
    (void)cls;char *s=native_utf8(e,code);int rc=s?dm_r2_connect(s):-1;ds_secret_free(s);return rc;
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_CloudSaves_command(JNIEnv *e,jclass cls,jstring action,jstring arg){
    (void)cls;char *a=native_utf8(e,action),*v=native_utf8(e,arg);
    if(a&&v){if(!strcmp(a,"show"))dm_sync_ui_open();else if(!strcmp(a,"disconnect"))dm_r2_disconnect();else if(!strcmp(a,"cancel"))dm_r2_cancel();
        else if(!strcmp(a,"background"))dm_r2_flush(1);else if(!strcmp(a,"foreground"))dm_r2_foreground();else dm_r2_command(a,v);}
    free(a);free(v);
}

JNIEXPORT jstring JNICALL Java_com_xiaoboluo_d2r_CloudSaves_text(JNIEnv *e,jclass cls,jstring key){
    (void)cls;char *k=native_utf8(e,key);jstring out=java_utf8(e,k?dm_ui(k):"");free(k);return out;
}

// 一次明确粘贴对应一次回调；不会在后台读取剪贴板。
// One explicit paste has one callback; clipboard is never read in the background.
static void (*paste_complete)(char *);
static pthread_mutex_t paste_mu=PTHREAD_MUTEX_INITIALIZER;
static void pasted_complete(char *text){pthread_mutex_lock(&paste_mu);void (*complete)(char*)=paste_complete;paste_complete=NULL;pthread_mutex_unlock(&paste_mu);if(complete)complete(text);else ds_secret_free(text);}
void ds_platform_paste(void (*complete)(char *)){pthread_mutex_lock(&paste_mu);paste_complete=complete;pthread_mutex_unlock(&paste_mu);if(!notify("paste"))pasted_complete(NULL);}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_CloudSaves_pasted(JNIEnv *e,jclass c,jstring code){
    (void)c;pasted_complete(code?native_utf8(e,code):NULL);
}
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_CloudSaves_gameUIReady(JNIEnv *e,jclass c){(void)e;(void)c;return dm_sync_ui_ready()!=0;}
