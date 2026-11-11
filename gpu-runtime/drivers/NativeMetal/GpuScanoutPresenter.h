/* SPDX-License-Identifier: MIT */
#pragma once
#import <Metal/Metal.h>
#include <unistd.h>
#include <new>
#include "../K70Display/NativeGOPWire.h"
#if !__has_feature(objc_arc)
#error K70 scanout ownership requires Objective-C ARC
#endif
namespace gpupresent {
enum Result {kOk,kBadFrame,kNoResource,kGpuUncertain};
struct Scanout {void *base;size_t bytes,stride;uint32_t width,height;};
class Presenter {
    struct Resources {
        id<MTLDevice> device;
        id<MTLCommandQueue> queue;
        id<MTLComputePipelineState> pipeline;
        id<MTLBuffer> target,sources[NGP_MAX_LAYERS];
        id<MTLCommandBuffer> pending;
    };
    Resources *resources_=new(std::nothrow) Resources{};
    Scanout scanout_;
    bool uncertain_=false;
    void retire() {resources_->pending=nil;for(auto &source:resources_->sources)source=nil;}
public:
    Presenter(const Presenter &)=delete;
    Presenter &operator=(const Presenter &)=delete;
    ~Presenter() {
        /* A caller may destroy the presenter after a timeout. Keep the entire
         * ARC owner alive in that case, not just dangling no-copy addresses.
         * One uncertain submission stops this presenter, bounding retention. */
        if(!uncertain_)delete resources_;
    }
    Presenter(id<MTLDevice> device,id<MTLLibrary> library,const Scanout &s):scanout_(s) {
        if(!resources_)return;
        resources_->device=device;
        const size_t page=(size_t)getpagesize();
        if(!s.base || ((uintptr_t)s.base&(page-1)) || !s.bytes || (s.bytes&(page-1)) ||
           s.width!=1440 || s.height!=3200 || s.stride!=5760 || uint64_t(s.stride)*s.height>s.bytes)return;
        id<MTLFunction> function=[library newFunctionWithName:@"k70_frame_scale"];
        if(!function)return;
        resources_->pipeline=[device newComputePipelineStateWithFunction:function error:nil];
        resources_->queue=[device newCommandQueue];
        if(!resources_->pipeline || !resources_->queue)return;
        resources_->target=[device newBufferWithBytesNoCopy:s.base length:s.bytes options:MTLResourceStorageModeShared deallocator:nil];
    }
    bool ready() const {return resources_ && resources_->target && resources_->pipeline && resources_->queue && !uncertain_;}
    Result present(const NGPScaleSwap &swap) {
        if(!ready())return uncertain_?kGpuUncertain:kNoResource;
        if(!swap.sequence || swap.version!=NGP_VERSION || !swap.count || swap.count>NGP_MAX_LAYERS)return kBadFrame;
        const uint64_t page=(uint64_t)getpagesize();
        for(unsigned i=0;i<swap.count;++i) {
            const auto &f=swap.layers[i];
            if(!f.address || (f.address&(page-1)) || !f.allocation_bytes || (f.allocation_bytes&(page-1)) ||
               f.allocation_bytes>SIZE_MAX || f.allocation_bytes>UINT64_MAX-f.address ||
               !f.source_bytes || f.source_bytes>f.allocation_bytes || !f.stride || (f.stride&3) ||
               f.lw!=1125 || f.lh!=2436 || f.pw!=scanout_.width || f.ph!=scanout_.height || f.reserved ||
               f.x0>=f.x1 || f.y0>=f.y1 || f.x1>f.pw || f.y1>f.ph ||
               f.lx>uint64_t(f.x0)*f.lw/f.pw || f.ly>uint64_t(f.y0)*f.lh/f.ph)return kBadFrame;
            uint64_t sx=uint64_t(f.x1-1)*f.lw/f.pw-f.lx,sy=uint64_t(f.y1-1)*f.lh/f.ph-f.ly;
            if((sx+1)*4>f.stride || sy*f.stride+(sx+1)*4>f.source_bytes)return kBadFrame;
        }
        @try {
            for(unsigned i=0;i<swap.count;++i) {
                const auto &f=swap.layers[i];
                resources_->sources[i]=[resources_->device newBufferWithBytesNoCopy:(void *)(uintptr_t)f.address length:(size_t)f.allocation_bytes
                    options:MTLResourceStorageModeShared deallocator:nil];
                if(!resources_->sources[i]) {retire();return kNoResource;}
            }
            resources_->pending=[resources_->queue commandBuffer];
            if(!resources_->pending) {retire();return kNoResource;}
            for(unsigned i=0;i<swap.count;++i) {
                const auto &f=swap.layers[i];
                /* Separate encoders preserve layer order through the native
                 * runtime's resource dependencies, including overlapping rects. */
                id<MTLComputeCommandEncoder> encoder=[resources_->pending computeCommandEncoder];
                if(!encoder) {retire();return kNoResource;}
                const NGPScaleConstants p{f.stride/4,uint32_t(scanout_.stride/4),f.lx,f.ly,
                    f.x0,f.y0,f.x1,f.y1,f.lw,f.lh,f.pw,f.ph};
                [encoder setComputePipelineState:resources_->pipeline];
                [encoder setBuffer:resources_->sources[i] offset:0 atIndex:0];
                [encoder setBuffer:resources_->target offset:0 atIndex:1];
                [encoder setBytes:&p length:sizeof p atIndex:2];
                const NSUInteger side=resources_->pipeline.maxTotalThreadsPerThreadgroup>=256?16:8;
                if(side*side>resources_->pipeline.maxTotalThreadsPerThreadgroup) {[encoder endEncoding];retire();return kNoResource;}
                [encoder dispatchThreadgroups:MTLSizeMake((f.x1-f.x0+side-1)/side,(f.y1-f.y0+side-1)/side,1)
                    threadsPerThreadgroup:MTLSizeMake(side,side,1)];
                [encoder endEncoding];
            }
            /* From this point on a failure cannot establish DMA quiescence.
             * Preserve all buffers and the command object on any uncertainty. */
            uncertain_=true;
            [resources_->pending commit];[resources_->pending waitUntilCompleted];
            if(resources_->pending.status!=MTLCommandBufferStatusCompleted)return kGpuUncertain;
            uncertain_=false;retire();return kOk;
        } @catch(NSException *exception) {
            (void)exception;if(uncertain_)return kGpuUncertain;
            retire();return kNoResource;
        }
    }
};
}
