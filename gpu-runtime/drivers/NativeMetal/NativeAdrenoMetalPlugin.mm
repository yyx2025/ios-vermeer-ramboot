/* SPDX-License-Identifier: MIT */
/* K70 iOS16: keep the GPU device available independently of scanout setup. */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach.h>
#include <time.h>
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
/* Bounded presenter setup retry for the fast-boot device-lost window. */
enum {K70_PRESENTER_TRIES=12,K70_PRESENTER_BACKOFF_S=1};
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
    char text[192];snprintf(text,sizeof text,"MTL %.12s %.20s %x %.128s",getprogname(),phase,status,detail);
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
struct Work {id<MTLDevice> device;NSBundle *bundle;unsigned port;Ref release;dispatch_semaphore_t prepared;
    ~Work(){if(prepared)dispatch_semaphore_signal(prepared);if(release)release(port);}};
void frontendReport(unsigned port) {
    char line[512]={};native_bridge_metal_status(line,sizeof line);
    report(port,"front-counts",0,line);
    if(strlen(line)>128)report(port,"front-detail",0,line+128);
    native_bridge_metal_formats(line,sizeof line);report(port,"front-formats",0,line);
    uint64_t stats[10]{};
    if(native_bridge_metal_work_stats(stats)) {
        snprintf(line,sizeof line,"pipeline %llu failed %llu warning %llu cache %llu/%llu/%llu",
            (unsigned long long)stats[0],(unsigned long long)stats[4],(unsigned long long)stats[6],
            (unsigned long long)stats[7],(unsigned long long)stats[8],(unsigned long long)stats[9]);
        report(port,"front-runtime",0,line);
    }
    nmb_last_warning(line,sizeof line);
    if(line[0]) {
        report(port,"front-warning",0,line);
        if(strlen(line)>128)report(port,"front-warning-tail",0,line+128);
    }
}
void *worker(void *argument) {
    auto *w=static_cast<Work *>(argument);
    @autoreleasepool { @try {
        const char *backend=getenv("K70_DISPLAY_BACKEND");
        const bool mappedDefault=!backend || !strcmp(backend,"mapped");
        report(w->port,mappedDefault?"mapped-start":"scanout-library");
        id<MTLLibrary> code=mappedDefault?nil:library(w->device,w->bundle);
        if(!mappedDefault && !code)report(w->port,"library-failed");
        void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
        auto *c=new Connection;c->call=io?(Call)dlsym(io,"IOConnectCallStructMethod"):nullptr;
        c->map=io?(Map)dlsym(io,"IOConnectMapMemory64"):nullptr;
        auto open=io?(Open)dlsym(io,"IOServiceOpen"):nullptr;
        const int status=open&&c->call&&c->map?open(w->port,mach_task_self(),0x4148,&c->connection):-1;
        if(status){report(w->port,"handoff-failed",status);delete c;delete w;return nullptr;}
        /* The fast (denoised) boot reaches presenter setup far earlier than
         * v11.41 did (t~977s vs t~8235s). At that point the GPU userland can
         * still be inside a transient device-lost window, so Presenter::ready()
         * fails even though the MODE/map handshake succeeded and other clients
         * later report device-ready. v11.41 only worked because it arrived here
         * hours later. Retry with bounded backoff: each attempt builds a fresh
         * Client/Presenter (new pipeline/queue/buffer) against the same device,
         * giving the userland time to recover instead of killing the worker on
         * the first refusal. */
        gpuscanout::Client *client=nullptr;bool opened=false;
        /* v11.43 raw-source probe: read the kernel snapshot the shader will
         * consume, before present(). report() is the same USB marker stream
         * that already carries presenter/front telemetry. */
        auto rawDiag=[w](uint64_t sequence,const NGPScaleSwap *swap) {
            if(!swap || !swap->count || !swap->layers[0].address)return;
            const NGPScaleLayer &f=swap->layers[0];
            if(!k70_wire_layer_valid(f,getpagesize(),1440,3200) || f.source_bytes<8)return;
            const uint8_t *snap=(const uint8_t *)(uintptr_t)f.address;
            const size_t midY=(f.source_bytes/f.stride)/2,midX=(f.stride/4)/2;
            const uint8_t *mid=snap+(size_t)f.stride*midY+midX*4;
            /* v11.44: a black scanout is the compositor's surface arriving empty. Sample 64 words spread over the
             * whole colour plane so "empty" is measured, not judged from three words. */
            uint32_t nonzero=0,sampled=0;
            const size_t bytes=(size_t)f.source_bytes;
            if(bytes>=4) {
                const size_t step=bytes/64&~3ull;
                for(size_t off=0;off+4<=bytes && sampled<64;off+=step?step:4) {if(*(const uint32_t *)(snap+off))++nonzero;++sampled;}
            }
            char line[192]{};
            snprintf(line,sizeof line,"seq %llu a %llx stride %u src %llu w0 %08x w1 %08x mid %08x nz %u/%u",
                (unsigned long long)sequence,(unsigned long long)f.address,f.stride,
                (unsigned long long)f.source_bytes,*(const uint32_t *)snap,
                *(const uint32_t *)(snap+4),*(const uint32_t *)mid,nonzero,sampled);
            report(w->port,"raw-color",0,line);
            const uint8_t *a=f.alpha_bytes && midY*f.alpha_stride+midX+4<=f.alpha_bytes?
                snap+f.alpha_offset+midY*f.alpha_stride+midX:nullptr;
            snprintf(line,sizeof line,"seq %llu aOff %llu aStride %u aMid %02x aQ %02x%02x%02x%02x",
                (unsigned long long)sequence,(unsigned long long)f.alpha_offset,f.alpha_stride,
                a?a[0]:0,a?a[0]:0,a?a[1]:0,a?a[2]:0,a?a[3]:0);
            report(w->port,"raw-alpha",0,line);
        };
        for(unsigned attempt=1;attempt<=K70_PRESENTER_TRIES && !opened;++attempt) {
            client=new gpuscanout::Client;
            client->diag=rawDiag;
            if(attempt==1)report(w->port,"presenter-create");
            opened=client->open(w->device,code,{c,call,map});
            if(!opened) {
                report(w->port,"presenter-retry",(int)attempt,nmb_last_error());
                delete client;client=nullptr;
                if(attempt<K70_PRESENTER_TRIES) {
                    struct timespec pause{K70_PRESENTER_BACKOFF_S,0};nanosleep(&pause,nullptr);
                }
            }
        }
        if(!opened){report(w->port,"presenter-failed",(int)K70_PRESENTER_TRIES);delete w;return nullptr;}
        report(w->port,"presenter-ready");
        /* Capture the old panel and prepare the lease before CoreAnimation
         * receives its device and can cause the kernel's first CPU clear. */
        dispatch_semaphore_signal(w->prepared);
        uint64_t frames=0;unsigned samples=0;
        while(client->serveOne()) {
            ++frames;
            if(samples<8 && (frames==1 || frames==120 || frames%600==0)) {
                ++samples;frontendReport(w->port);
            }
        }
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
            dispatch_semaphore_t prepared=dispatch_semaphore_create(0);
            auto *w=new Work{(id<MTLDevice>)device,bundle,port,release,prepared};pthread_attr_t attributes;pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes,8u<<20);pthread_t thread;
            const int status=pthread_create(&thread,&attributes,worker,w);pthread_attr_destroy(&attributes);
            if(status){report(port,"worker-failed",status);delete w;}
            else {
                pthread_detach(thread);
                const long waited=dispatch_semaphore_wait(prepared,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC));
                if(waited)report(port,"mapped-start-timeout");
            }
        } else report(port,"port-retain-fail");
    }
    report(port,"device-returned");return device;
}
}
@implementation NativeAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self=[super init];if(!self || !port)return nil;
    @synchronized([NativeAdrenoMetalDevice class]) {
        /* v42 previously latched nil forever after the first device-lost
         * attempt, so a transient VK_ERROR_DEVICE_LOST kept backboardd from
         * ever reaching presenter ready -> arm -> GPU completed (E1/E2/E3).
         * Cache success only and allow bounded retries, matching v11.41's
         * effective ability to recover its presenter path. */
        static unsigned attempts=0;static id device=nil;
        if(device)return device;
        if(attempts>=8)return nil;
        const unsigned attempt=++attempts;
        @try {device=open(port);} @catch(NSException *exception) {report(port,"open-exception",0,exception.name.UTF8String?:"?");device=nil;}
        if(!device)report(port,"device-retry",(int)attempt,nmb_last_error());
        return device;
    }
}
@end
