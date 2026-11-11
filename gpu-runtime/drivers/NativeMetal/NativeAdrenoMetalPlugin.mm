/* SPDX-License-Identifier: MIT */
/* K70 iOS16 discovery entry. Only the frame-serving worker runs. No probe,
 * telemetry, private QuartzCore address access or shader benchmark is used. */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach.h>
#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "GpuScanoutClient.h"
@interface _MTLDevice : NSObject
@end
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
namespace {
using Call=int (*)(unsigned,uint32_t,const void *,size_t,void *,size_t *);
using Map=int (*)(unsigned,uint32_t,unsigned,uint64_t *,uint64_t *,uint32_t);
struct ScanoutConnection {unsigned connection=0;Call call=nullptr;Map map=nullptr;};
int call(void *cookie,uint32_t op,const void *input,size_t n,void *output,size_t bytes) {
    auto *c=static_cast<ScanoutConnection *>(cookie);
    if(n>sizeof(NGPComplete) || !c->call || !c->connection)return -1;
    uint32_t request[2+sizeof(NGPComplete)/4]{};request[0]=0x4e544e4c;request[1]=0x7000+op;
    if(n)memcpy(request+2,input,n);
    size_t actual=bytes;
    int status=c->call(c->connection,1,request,n+8,output,&actual);
    return status?status:actual==bytes?0:-1;
}
int map(void *cookie,uint64_t *address,uint64_t *bytes) {
    auto *c=static_cast<ScanoutConnection *>(cookie);
    return c->map?c->map(c->connection,0x5343,mach_task_self(),address,bytes,1 /* anywhere */):-1;
}
id<MTLLibrary> scaleLibrary(id<MTLDevice> device,NSBundle *bundle) {
    NSString *path=[bundle pathForResource:@"K70FrameScale" ofType:@"metallib"];
    NSData *data=path?[NSData dataWithContentsOfFile:path]:nil;
    if(!data.length)return nil;
    dispatch_data_t code=dispatch_data_create(data.bytes,data.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)data;});
    return [device newLibraryWithData:code error:nil];
}
id open(unsigned port) {
    NSBundle *bundle=[NSBundle bundleForClass:[NativeAdrenoMetalDevice class]];
    NSString *turnip=[bundle pathForResource:@"libvulkan_freedreno" ofType:@"dylib"];
    if(!turnip)return nil;
    uint64_t registryID=0;
    void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!io)return nil;
    auto entry=(int (*)(unsigned,uint64_t *))dlsym(io,"IORegistryEntryGetRegistryEntryID");
    if(!entry || entry(port,&registryID))return nil;
    /* Public process preferences select ordinary BGRA rows for this port. */
    setenv("TU_DEBUG","sysmem,nocb",0);
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION","1",1);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    id device=nil;
    for(NSString *root in NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES)) {
        NSString *cache=[[root stringByAppendingPathComponent:@"NativeAdrenoMetal"] stringByAppendingPathComponent:
            [NSString stringWithUTF8String:getprogname()]];
        if(![[NSFileManager defaultManager] createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil])continue;
        device=native_bridge_metal_open(turnip.fileSystemRepresentation,cache.fileSystemRepresentation,registryID,nullptr);
        if(device)break;
    }
    if(!device || strcmp(getprogname(),"backboardd"))return device;
    id<MTLLibrary> library=scaleLibrary((id<MTLDevice>)device,bundle);
    if(!library)return nil;
    auto *connection=new ScanoutConnection;
    connection->call=(Call)dlsym(io,"IOConnectCallStructMethod");connection->map=(Map)dlsym(io,"IOConnectMapMemory64");
    auto serviceOpen=(int (*)(unsigned,unsigned,uint32_t,unsigned *))dlsym(io,"IOServiceOpen");
    if(!serviceOpen || !connection->call || !connection->map || serviceOpen(port,mach_task_self(),0x4148,&connection->connection))return nil;
    auto *client=new gpuscanout::Client;
    if(!client->open((id<MTLDevice>)device,library,{connection,call,map}))return nil;
    pthread_t worker;
    if(pthread_create(&worker,nullptr,[](void *p)->void * {
        auto *c=static_cast<gpuscanout::Client *>(p);
        while(c->serveOne()) {}
        /* Keep presenter/connection alive on an uncertain GPU result. */
        return nullptr;
    },client))return nil;
    pthread_detach(worker);return device;
}
}
@implementation NativeAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self=[super init];if(!self || !port)return nil;
    @synchronized([NativeAdrenoMetalDevice class]) {
        static bool attempted=false;static id device=nil;
        if(attempted)return device;attempted=true;
        @try {device=open(port);} @catch(NSException *exception) {(void)exception;device=nil;}
        return device;
    }
}
@end
