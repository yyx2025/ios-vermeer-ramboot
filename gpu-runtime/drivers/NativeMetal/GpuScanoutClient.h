/* SPDX-License-Identifier: MIT */
#pragma once
#include <functional>
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
public:
    /* v11.43 raw-source probe. Called once per sampled frame, BEFORE present(),
     * with the kernel snapshot the shader is about to read. The producer dump
     * must show these bytes if the handoff copy is correct; all-zero here with
     * continuing CoreAnimation swaps means the source surface is empty. */
    std::function<void(uint64_t,const NGPScaleSwap *)> diag;
    uint64_t diag_frames_=0;
    bool open(id<MTLDevice> device,id<MTLLibrary> library,const Transport &t) {
        if(!t.call || !t.mapScanout || !device || !library)return false;
        transport_=t;NGPMode mode{};
        if(t.call(t.context,NGP_METHOD_MODE,nullptr,0,&mode,sizeof mode) || mode.version!=NGP_VERSION ||
           mode.format!=NGP_BGRX8888 || mode.width!=1440 || mode.height!=3200 || mode.stride!=5760 ||
           mode.bytes<mode.stride*mode.height || mode.bytes>SIZE_MAX)return false;
        uint64_t address=0,bytes=0;
        if(t.mapScanout(t.context,&address,&bytes) || bytes<mode.bytes)return false;
        presenter_=new gpupresent::Presenter(device,library,{(void *)(uintptr_t)address,(size_t)mode.bytes,
            (size_t)mode.stride,(uint32_t)mode.width,(uint32_t)mode.height});
        if(!presenter_->ready())return false;
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
        if(diag && swap.sequence && swap.count && (diag_frames_<8 || diag_frames_%600==0)) {
            ++diag_frames_;diag(swap.sequence,&swap);
        }
        gpupresent::Result result;
        @autoreleasepool {result=presenter_->present(swap);}
        const NGPComplete done{swap.sequence,result==gpupresent::kOk?0u:0xe00002bcu,
            result==gpupresent::kGpuUncertain?0u:NGP_QUIESCENT};
        const int completed=transport_.call(transport_.context,NGP_METHOD_COMPLETE_SWAP,&done,sizeof done,nullptr,0);
        return !completed && result==gpupresent::kOk;
    }
};
}
