#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif
#include "dm_sync.h"
#include "dm_sync_ui.h"
#include "../host/dm_i18n.h"
char *ds_path_key(const char *path){@autoreleasepool{
    NSString *s=[[NSString alloc]initWithBytes:path length:strlen(path) encoding:NSUTF8StringEncoding];
    if(!s)return NULL;
    return strdup([[s precomposedStringWithCanonicalMapping] lowercaseStringWithLocale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]].UTF8String);
}}

@interface DSSaveRequest : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSMutableData *data;
@property(nonatomic, strong) dispatch_semaphore_t done;
@property NSInteger code;
@property int64_t expected;
@property NSInteger percent;
@end
@implementation DSSaveRequest
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)r completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    self.code=[(NSHTTPURLResponse *)r statusCode];
    self.expected=r.expectedContentLength;self.percent=-1;
    completion(r.expectedContentLength>DS_LIMIT?NSURLSessionResponseCancel:NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    if(self.data.length+data.length>DS_LIMIT){self.code=-1;[task cancel];return;}[self.data appendData:data];
    NSInteger percent=self.expected>0?(NSInteger)(100*self.data.length/self.expected):(NSInteger)(self.data.length/16384);
    if(percent!=self.percent){self.percent=percent;dm_r2_progress(self.data.length,self.expected>0?(size_t)self.expected:0);}
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    if(error)self.code=-1;dispatch_semaphore_signal(self.done);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    completion(nil); // Credentials must never follow redirects to another origin.
}
@end
static NSURLSessionTask *activeTask;
void ds_transport_cancel(void){@synchronized(DSSaveRequest.class){[activeTask cancel];}}
int ds_transport(const char *method,const char *url,const char *token,const void *body,size_t size,unsigned ms,unsigned char **out,size_t *n){
    @autoreleasepool {
        *out=NULL;*n=0;
        NSURL *u=[NSURL URLWithString:@(url)];if(!u||u.user||u.password)return -1;
        if(![u.scheme isEqualToString:@"https"]&&!(getenv("DM_SYNC_TEST")&&[u.host isEqualToString:@"localhost"]))return -1;
        NSMutableURLRequest *r=[NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:ms/1000.0];
        r.HTTPMethod=@(method);[r setValue:[@"Bearer " stringByAppendingString:@(token)] forHTTPHeaderField:@"Authorization"];
        [r setValue:strstr(url,"/snapshots/")?@"application/zip":@"application/json" forHTTPHeaderField:@"Content-Type"];
        if(body)r.HTTPBody=[NSData dataWithBytes:body length:size];
        DSSaveRequest *delegate=[DSSaveRequest new];delegate.data=[NSMutableData new];delegate.done=dispatch_semaphore_create(0);delegate.code=-1;
        NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.timeoutIntervalForRequest=ms/1000.0;config.timeoutIntervalForResource=ms/1000.0;config.URLCache=nil;
        NSURLSession *session=[NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:nil];
        NSURLSessionDataTask *task=[session dataTaskWithRequest:r];@synchronized(DSSaveRequest.class){activeTask=task;}[task resume];
        long timeout=dispatch_semaphore_wait(delegate.done,dispatch_time(DISPATCH_TIME_NOW,(int64_t)ms*NSEC_PER_MSEC));
        if(timeout){[task cancel];[session invalidateAndCancel];@synchronized(DSSaveRequest.class){if(activeTask==task)activeTask=nil;}return -1;}
        @synchronized(DSSaveRequest.class){if(activeTask==task)activeTask=nil;}
        [session invalidateAndCancel];
        if(delegate.code<0||timeout)return -1;
        *n=delegate.data.length;*out=malloc(*n+1);if(!*out)return -1;
        memcpy(*out,delegate.data.bytes,*n);(*out)[*n]=0;return (int)delegate.code;
    }
}
#ifndef DM_SYNC_TEST_DRIVER
static NSDictionary *credentialQuery(void){
    return @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService:@"com.icechriszen.d2r.save-sync",
        (__bridge id)kSecAttrAccount:getenv("DM_SYNC_TEST")?@"device-connection.test":@"device-connection",
        (__bridge id)kSecAttrSynchronizable:@NO};
}
char *ds_credentials_load(void){
    NSMutableDictionary *q=[credentialQuery() mutableCopy];q[(__bridge id)kSecReturnData]=@YES;
    CFTypeRef result=NULL;OSStatus s=SecItemCopyMatching((__bridge CFDictionaryRef)q,&result);
    if(s!=errSecSuccess)return NULL;NSData *d=CFBridgingRelease(result);
    NSString *v=[[NSString alloc]initWithData:d encoding:NSUTF8StringEncoding];return v?strdup(v.UTF8String):NULL;
}
int ds_credentials_store(const char *json){
    NSDictionary *q=credentialQuery();if(!json){OSStatus s=SecItemDelete((__bridge CFDictionaryRef)q);return s==errSecSuccess||s==errSecItemNotFound?0:-1;}
    NSData *d=[@(json) dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *attrs=@{(__bridge id)kSecValueData:d,(__bridge id)kSecAttrAccessible:(__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus s=SecItemUpdate((__bridge CFDictionaryRef)q,(__bridge CFDictionaryRef)attrs);
    if(s==errSecItemNotFound){NSMutableDictionary *add=[q mutableCopy];[add addEntriesFromDictionary:attrs];s=SecItemAdd((__bridge CFDictionaryRef)add,NULL);}
    if(s!=errSecSuccess)fprintf(stderr,"[r2] secure credential store status=%d\n",(int)s);
    return s==errSecSuccess?0:-1;
}
static NSDictionary *stateView(void){
    char *s=dm_r2_status();NSData *d=[NSData dataWithBytes:s length:strlen(s)];free(s);
    return [NSJSONSerialization JSONObjectWithData:d options:0 error:nil]?:@{};
}

// 仅启动期间保留原生下载提示；运行期界面完全交给游戏控件。
// Keep native download progress only during bootstrap; runtime UI belongs to game widgets.
void ds_platform_paste(void (*complete)(char *)){
    dispatch_async(dispatch_get_main_queue(),^{
#if TARGET_OS_IPHONE
        NSString *value=UIPasteboard.generalPasteboard.string;
#else
        NSString *value=[NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
#endif
        const char *bytes=value.UTF8String;complete(bytes&&strlen(bytes)<=4096?strdup(bytes):NULL);
    });
}
#if TARGET_OS_IPHONE
static UIAlertController *startupProgress;
static UIViewController *presenter(void){
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)if([scene isKindOfClass:UIWindowScene.class])
        for(UIWindow *w in ((UIWindowScene *)scene).windows)if(w.isKeyWindow){UIViewController *v=w.rootViewController;while(v.presentedViewController)v=v.presentedViewController;return v;}
    return nil;
}
#else
static NSPanel *startupProgress;static NSTextField *startupLabel;
@interface DSSyncCancel : NSObject
- (void)cancel:(id)sender;
@end
@implementation DSSyncCancel
- (void)cancel:(id)sender{(void)sender;dm_r2_cancel();}
@end
#endif
void ds_platform_show(void){dm_sync_ui_open();}
void ds_platform_changed(void){dm_sync_ui_changed();dispatch_async(dispatch_get_main_queue(),^{
    NSDictionary *s=stateView();BOOL download=!dm_sync_ui_ready()&&[s[@"status"] isEqual:@"downloading"];
    double total=[s[@"total"] doubleValue],got=[s[@"received"] doubleValue];
    NSString *label=[NSString stringWithFormat:@(dm_ui(total>0?"dmSyncDownloadPercentAndroid":"dmSyncDownloadKBAndroid")),total>0?(int)MIN(100,100*got/total):(int)(got/1024)];
#if TARGET_OS_IPHONE
    if(!download){if(startupProgress)[startupProgress dismissViewControllerAnimated:NO completion:nil];startupProgress=nil;return;}
    if(!startupProgress){UIViewController *v=presenter();if(!v||v.presentingViewController)return;
        startupProgress=[UIAlertController alertControllerWithTitle:@(dm_ui("dmSyncDownloadTitle")) message:label preferredStyle:UIAlertControllerStyleAlert];
        [startupProgress addAction:[UIAlertAction actionWithTitle:@(dm_ui("dmSyncCancelOffline")) style:UIAlertActionStyleCancel handler:^(UIAlertAction *a){dm_r2_cancel();}]];
        [v presentViewController:startupProgress animated:NO completion:nil];}
    startupProgress.message=label;
#else
    if(!download){[startupProgress close];startupProgress=nil;startupLabel=nil;return;}
    if(!startupProgress){startupProgress=[[NSPanel alloc]initWithContentRect:NSMakeRect(0,0,460,130) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];startupProgress.releasedWhenClosed=NO;
        startupProgress.title=@(dm_ui("dmSyncDownloadTitle"));startupLabel=[NSTextField labelWithString:label];startupLabel.frame=NSMakeRect(20,75,420,35);[startupProgress.contentView addSubview:startupLabel];
        static DSSyncCancel *target;target=[DSSyncCancel new];NSButton *cancel=[NSButton buttonWithTitle:@(dm_ui("dmSyncCancelOffline")) target:target action:@selector(cancel:)];cancel.frame=NSMakeRect(190,20,250,35);[startupProgress.contentView addSubview:cancel];[startupProgress center];[startupProgress makeKeyAndOrderFront:nil];}
    startupLabel.stringValue=label;
#endif
});}
#endif
