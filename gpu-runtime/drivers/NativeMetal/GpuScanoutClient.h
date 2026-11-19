/* SPDX-License-Identifier: MIT */
#pragma once
#include <functional>
#include <cstdlib>
#include <cstring>
#include "GpuScanoutPresenter.h"
namespace gpuscanout {
struct Transport {
    void *context;
    int (*call)(void *,uint32_t,const void *,size_t,void *,size_t);
    int (*mapScanout)(void *,uint64_t *,uint64_t *);
};
class Client {
    Transport transport_{};
    gpupresent::Presenter *presenter_=nullptr;
    k70mapped::Driver *mapped_=nullptr;
    uint64_t address_=0,bytes_=0;
    bool useMapped_=false;
public:
    ~Client(){delete presenter_;delete mapped_;}
    /* v11.43 raw-source probe. Called once per sampled frame, BEFORE present(),
     * with the kernel snapshot the shader is about to read. The producer dump
     * must show these bytes if the handoff copy is correct; all-zero here with
     * continuing CoreAnimation swaps means the source surface is empty. */
    std::function<void(uint64_t,const NGPScaleSwap *)> diag;
    uint64_t diag_frames_=0,empty_frames_=0,mapped_frames_=0;
    bool open(id<MTLDevice> device,id<MTLLibrary> library,const Transport &t) {
        if(!t.call || !t.mapScanout)return false;
        transport_=t;NGPMode mode{};
        if(t.call(t.context,NGP_METHOD_MODE,nullptr,0,&mode,sizeof mode) || mode.version!=NGP_VERSION ||
           mode.format!=NGP_BGRX8888 || mode.width!=1440 || mode.height!=3200 || mode.stride!=5760 ||
           mode.bytes<mode.stride*mode.height || mode.bytes>SIZE_MAX)return false;
        uint64_t address=0,bytes=0;
        if(t.mapScanout(t.context,&address,&bytes) || bytes<mode.bytes)return false;
        address_=address;bytes_=mode.stride*mode.height;
        mapped_=new k70mapped::Driver({(void *)(uintptr_t)address,(size_t)mode.bytes,
            (size_t)mode.stride,(uint32_t)mode.width,(uint32_t)mode.height},getpagesize());
        if(!mapped_->ready())return false;
        const char *backend=getenv("K70_DISPLAY_BACKEND");
        /* Default this repair release to the independent mapped presenter.
         * Setting gpu keeps the staged GPU path available for comparison. */
        useMapped_=!backend || !strcmp(backend,"mapped");
        if(!useMapped_ && device && library) {
            presenter_=new gpupresent::Presenter(device,library,{(void *)(uintptr_t)address,(size_t)mode.bytes,
                (size_t)mode.stride,(uint32_t)mode.width,(uint32_t)mode.height});
        }
        if(!presenter_ || !presenter_->ready())useMapped_=true;
        NSLog(@"native-metal: display backend %@",useMapped_?@"mapped":@"gpu-staging");
        const NGPArm arm{NGP_VERSION,0};
        return t.call(t.context,NGP_METHOD_ARM,&arm,sizeof arm,nullptr,0)==0;
    }
    /* Waits only for offered work. No monitor, image readback or periodic
     * telemetry. Completion flags distinguish pre-commit refusal from DMA
     * uncertainty; the latter permanently stops this client. */
    bool serveOne() {
        const NGPWait wait{1000};NGPScaleSwap swap{};
        int status=transport_.call(transport_.context,NGP_METHOD_WAIT_SWAP,&wait,sizeof wait,&swap,sizeof swap);
        if((uint32_t)status==0xe00002d6u)return true;
        if(status)return false;
        ++diag_frames_;
        if(diag && swap.sequence && swap.count && (diag_frames_<=8 || diag_frames_%600==0))diag(swap.sequence,&swap);
        gpupresent::Result result;
        @autoreleasepool {result=useMapped_?gpupresent::kNoResource:presenter_->present(swap);}
        /* Only a refusal before any GPU commit may use CPU fallback. Uncertain
         * DMA keeps the kernel lease blocked and cannot race mapped writes. */
        if(useMapped_ || result==gpupresent::kNoResource) {
            useMapped_=true;
            const auto cpu=mapped_->present(swap);
            if(cpu==k70mapped::kPresented) {
                sys_dcache_flush((void *)(uintptr_t)address_,bytes_);
                ++mapped_frames_;
                if(mapped_frames_<=8 || mapped_frames_%120==0)
                    NSLog(@"native-metal: mapped completed sequence %llu",(unsigned long long)swap.sequence);
                result=gpupresent::kOk;
            } else if(cpu==k70mapped::kEmpty)result=gpupresent::kEmpty;
            else result=cpu==k70mapped::kInvalid?gpupresent::kBadFrame:gpupresent::kNoResource;
        }
        /* An empty source is completed as a successful swap with no GPU work: the scanout keeps the boot logo instead of
         * being cleared to black, and the next swap with real content presents normally. */
        const bool presented=result==gpupresent::kOk||result==gpupresent::kEmpty;
        if(result==gpupresent::kEmpty && empty_frames_<8) {
            ++empty_frames_;
            NSLog(@"native-metal: empty source %llu kept on scanout",(unsigned long long)swap.sequence);
        }
        const NGPComplete done{swap.sequence,presented?0u:0xe00002bcu,
            result==gpupresent::kGpuUncertain?0u:NGP_QUIESCENT};
        const int completed=transport_.call(transport_.context,NGP_METHOD_COMPLETE_SWAP,&done,sizeof done,nullptr,0);
        return !completed && presented;
    }
};
}
