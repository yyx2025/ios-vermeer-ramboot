/* SPDX-License-Identifier: MIT */
/* K70 iOS16: keep the GPU device available independently of scanout setup. */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach.h>
#include <cstring>
#include <cstdio>
#include "NativeBridgeMetalFrontend.h"
#include "GpuScanoutClient.h"
#include "SharedShaderCache.h"
#include "bridge/NativeMetalObjects.h"
@interface _MTLDevice : NSObject
@end
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
namespace {
using Call=int (*)(unsigned,uint32_t,const void *,size_t,void *,size_t *);
using Map=int (*)(unsigned,uint32_t,unsigned,uint64_t *,uint64_t *,uint32_t);
using Open=int (*)(unsigned,unsigned,uint32_t,unsigned *);
using Ref=int (*)(unsigned);
struct Connection {unsigned connection=0;Call call=nullptr;Map map=nullptr;};
/* Existing marker stream works even before a GPU connection exists. */
void report(unsigned port,const char *phase,int status=0,const char *detail="") {
    void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    auto open=io?(Open)dlsym(io,"IOServiceOpen"):nullptr;
    auto close=io?(Ref)dlsym(io,"IOServiceClose"):nullptr;
    if(!open)return;
    char text[64];snprintf(text,sizeof text,"MTL %.12s %.20s %x %.16s",getprogname(),phase,status,detail);
    auto mark=[&](unsigned value){unsigned c=0;open(port,mach_task_self(),0x41470000u|value,&c);if(c&&close)close(c);};
    mark(0x7ffe);for(const char *p=text;*p;++p)mark(0x7000|(unsigned char)*p);mark(0x700a);
}
int call(void *cookie,uint32_t op,const void *input,size_t n,void *output,size_t bytes) {
    auto *c=static_cast<Connection *>(cookie);
    if(n>sizeof(NGPComplete) || !c->call || !c->connection)return -1;
    uint32_t request[2+sizeof(NGPComplete)/4]{};request[0]=0x4e544e4c;request[1]=0x7000+op;
    if(n)memcpy(request+2,input,n);size_t actual=bytes;
    const int status=c->call(c->connection,1,request,n+8,output,&actual);
    return status?status:actual==bytes?0:-1;
}
int map(void *cookie,uint64_t *address,uint64_t *bytes) {
    auto *c=static_cast<Connection *>(cookie);
    return c->map?c->map(c->connection,0x5343,mach_task_self(),address,bytes,1):-1;
}
id<MTLLibrary> library(id<MTLDevice> device,NSBundle *bundle) {
    NSString *path=[bundle pathForResource:@"K70FrameScale" ofType:@"metallib"];
    NSData *data=path?[NSData dataWithContentsOfFile:path]:nil;if(!data.length)return nil;
    dispatch_data_t code=dispatch_data_create(data.bytes,data.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)data;});
    return [device newLibraryWithData:code error:nil];
}
struct Work {id<MTLDevice> device;NSBundle *bundle;unsigned port;Ref release;~Work(){if(release)release(port);}};
void *worker(void *argument) {
    auto *w=static_cast<Work *>(argument);
    @autoreleasepool { @try {
        report(w->port,"scanout-library");id<MTLLibrary> code=library(w->device,w->bundle);
        if(!code){report(w->port,"library-failed");delete w;return nullptr;}
        void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
        auto *c=new Connection;c->call=io?(Call)dlsym(io,"IOConnectCallStructMethod"):nullptr;
        c->map=io?(Map)dlsym(io,"IOConnectMapMemory64"):nullptr;
        auto open=io?(Open)dlsym(io,"IOServiceOpen"):nullptr;
        const int status=open&&c->call&&c->map?open(w->port,mach_task_self(),0x4148,&c->connection):-1;
        if(status){report(w->port,"handoff-failed",status);delete c;delete w;return nullptr;}
        auto *client=new gpuscanout::Client;report(w->port,"presenter-create");
        if(!client->open(w->device,code,{c,call,map})) {
            report(w->port,"presenter-failed");delete w;return nullptr;
        }
        report(w->port,"presenter-ready");while(client->serveOne()) {}
        /* Keep presenter/connection resources on uncertain GPU completion. */
        report(w->port,"presenter-stopped");
    } @catch(NSException *exception) {report(w->port,"scanout-exception",0,exception.name.UTF8String?:"?");}}
    delete w;return nullptr;
}
id open(unsigned port) {
    report(port,"discovery");NSBundle *bundle=[NSBundle bundleForClass:[NativeAdrenoMetalDevice class]];
    NSString *turnip=[bundle pathForResource:@"libvulkan_freedreno" ofType:@"dylib"];
    if(!turnip){report(port,"turnip-missing");return nil;}
    uint64_t registryID=0;void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!io){report(port,"iokit-missing");return nil;}
    auto entry=(int (*)(unsigned,uint64_t *))dlsym(io,"IORegistryEntryGetRegistryEntryID");
    if(!entry || entry(port,&registryID)){report(port,"registry-failed");return nil;}
    setenv("TU_DEBUG","sysmem,nocb",0);setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION","1",1);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    std::vector<std::string> candidates;const std::string shared=sharedShaderCacheDirectory();
    if(!shared.empty())candidates.push_back(shared);
    for(NSString *root in NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES))
        candidates.push_back([[root stringByAppendingPathComponent:@"NativeAdrenoMetal"] fileSystemRepresentation]);
    if(NSString *temporary=NSTemporaryDirectory())candidates.push_back(temporary.fileSystemRepresentation);
    candidates.push_back("/tmp");candidates.push_back("/var/tmp");id device=nil;
    for(const auto &root:candidates) {
        const std::string cache=root+"/K70-metal-v37-u"+std::to_string(getuid());NSString *path=[NSString stringWithUTF8String:cache.c_str()];
        if(![[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil])continue;
        struct stat st{};if(lstat(cache.c_str(),&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&0022))continue;
        std::string probe=cache+"/.write-XXXXXX";int fd=mkstemp(probe.data());if(fd<0)continue;close(fd);unlink(probe.c_str());
        configureShaderDiskCache(cache);setenv("MESA_SHADER_CACHE_MAX_SIZE","64M",0);report(port,"device-create");
        device=native_bridge_metal_open(turnip.fileSystemRepresentation,cache.c_str(),registryID,nullptr);if(device)break;
    }
    if(!device){report(port,"device-failed",0,nmb_last_error());return nil;}report(port,"device-ready");
    if(!strcmp(getprogname(),"backboardd")) {
        auto retain=(Ref)dlsym(io,"IOObjectRetain");auto release=(Ref)dlsym(io,"IOObjectRelease");
        if(retain && release && !retain(port)) {
            auto *w=new Work{(id<MTLDevice>)device,bundle,port,release};pthread_attr_t attributes;pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes,8u<<20);pthread_t thread;
            const int status=pthread_create(&thread,&attributes,worker,w);pthread_attr_destroy(&attributes);
            if(status){report(port,"worker-failed",status);delete w;}else pthread_detach(thread);
        } else report(port,"port-retain-fail");
    }
    report(port,"device-returned");return device;
}
}
@implementation NativeAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self=[super init];if(!self || !port)return nil;
    @synchronized([NativeAdrenoMetalDevice class]) {
        static bool attempted=false;static id device=nil;if(attempted)return device;attempted=true;
        @try {device=open(port);} @catch(NSException *exception) {report(port,"open-exception",0,exception.name.UTF8String?:"?");device=nil;}
        return device;
    }
}
@end
