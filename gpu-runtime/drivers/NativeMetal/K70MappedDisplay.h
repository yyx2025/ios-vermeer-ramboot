/* SPDX-License-Identifier: MIT */
#pragma once
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include "K70FrameWireCheck.h"
#include "K70PixelMath.h"

/* The mapped display backend uses the existing exclusive WAIT/COMPLETE lease.
 * It only publishes completed real surface contents. GPU DMA never owns the
 * physical framebuffer in this backend. */
namespace k70mapped {
struct Scanout {void *base;size_t bytes,stride;uint32_t width,height;};
enum Result {kPresented,kEmpty,kInvalid,kNoMemory};
static inline bool zeroBytes(const uint8_t *p,size_t bytes) {
    while(bytes>=sizeof(uint64_t)) {
        uint64_t word;std::memcpy(&word,p,sizeof word);
        if(word)return false;
        p+=sizeof word;bytes-=sizeof word;
    }
    while(bytes--)if(*p++)return false;
    return true;
}
static inline bool empty(const NGPScaleSwap &swap) {
    for(unsigned i=0;i<swap.count;++i) {
        const auto &f=swap.layers[i];
        const auto *base=reinterpret_cast<const uint8_t *>(uintptr_t(f.address));
        if(!zeroBytes(base,size_t(f.source_bytes)))return false;
        /* An opaque black frame has nonzero alpha, including B3A8's separate
         * plane. Sparse color sampling must never classify it as uninitialized. */
        if(f.alpha_bytes && !zeroBytes(base+f.alpha_offset,size_t(f.alpha_bytes)))return false;
    }
    return true;
}
static inline bool valid(const NGPScaleSwap &swap,const Scanout &s,uint64_t page) {
    if(!page || (page&(page-1)) || !s.base || (uintptr_t(s.base)&(page-1)) ||
       !s.bytes || (s.bytes&(page-1)) || s.width!=1440 || s.height!=3200 || s.stride!=5760 ||
       uint64_t(s.stride)*s.height>s.bytes || !swap.sequence || swap.version!=NGP_VERSION ||
       !swap.count || swap.count>NGP_MAX_LAYERS)return false;
    for(unsigned i=0;i<swap.count;++i)
        if(!k70_wire_layer_valid(swap.layers[i],page,s.width,s.height))return false;
    return true;
}
/* Geometry and premultiplied source-over match the v3 display contract.
 * The union exterior retains the last frame; first coverage starts over black. */
static inline void render(const NGPScaleSwap &swap,uint32_t *pixels,size_t stride) {
    const size_t targetStride=stride/4;
    for(unsigned i=0;i<swap.count;++i) {
        const auto &f=swap.layers[i];
        const auto *source=reinterpret_cast<const uint8_t *>(uintptr_t(f.address));
        for(uint32_t y=f.y0;y<f.y1;++y) {
            const uint64_t sy=uint64_t(y)*f.lh/f.ph-f.ly;
            for(uint32_t x=f.x0;x<f.x1;++x) {
                const uint64_t sx=uint64_t(x)*f.lw/f.pw-f.lx;
                uint32_t word;std::memcpy(&word,source+sy*f.stride+sx*4,sizeof word);
                uint32_t &destination=pixels[size_t(y)*targetStride+x];
                if(f.format==NGP_SOURCE_BGRA)destination=word;
                else {
                    bool first=true;
                    for(unsigned n=0;n<i;++n) {
                        const auto &p=swap.layers[n];
                        if(x>=p.x0 && x<p.x1 && y>=p.y0 && y<p.y1){first=false;break;}
                    }
                    destination=k70_xr10_bgra(word,source[f.alpha_offset+sy*f.alpha_stride+sx],destination,first);
                }
            }
        }
    }
}
class Driver {
    Scanout scanout_{};
    uint64_t page_;
    std::unique_ptr<uint32_t[]> staging_;
public:
    Driver(const Scanout &s,uint64_t page):scanout_(s),page_(page) {
        if(s.bytes && s.bytes<=256u<<20)staging_.reset(new(std::nothrow) uint32_t[s.bytes/4]);
    }
    bool ready() const {return staging_!=nullptr;}
    Result present(const NGPScaleSwap &swap) {
        if(!valid(swap,scanout_,page_))return kInvalid;
        if(!ready())return kNoMemory;
        if(empty(swap))return kEmpty;
        std::memcpy(staging_.get(),scanout_.base,scanout_.bytes);
        render(swap,staging_.get(),scanout_.stride);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(scanout_.base,staging_.get(),scanout_.stride*scanout_.height);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return kPresented;
    }
};
}
