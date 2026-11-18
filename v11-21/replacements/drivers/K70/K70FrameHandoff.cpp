#include "K70BootLog.h"
/* SPDX-License-Identifier: MIT */
#include "K70FrameHandoff.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#if !defined(K70_FRAME_HOST_TEST)
#include <machine/machine_routines.h>
#include <kern/cpu_data.h>
#endif
#include <string.h>
extern "C" void proc_selfname(char *,int);
extern "C" int vermeer_console_muted;
namespace {
constexpr size_t kSnapshotBytes=(size_t(1125)*2436*5+31+16383)&~size_t(16383);
struct Handoff {
    IOLock *lock=nullptr;
    IOMemoryDescriptor *scanout=nullptr;
    IOBufferMemoryDescriptor *snapshot[NGP_MAX_LAYERS]{};
    IOMemoryMap *views[NGP_MAX_LAYERS]{};
    task_t owner=TASK_NULL;
    NGPMode mode{};NGPScaleSwap frame{};K70FrameGate gate{};
    uint64_t completed=0;
    volatile uint8_t *pixels=nullptr;
    uint32_t expected[512]{};
    uint32_t diagnostics=0,alphaMin=255,alphaMax=0,format=0,snapshotRefusals=0;
    uint32_t sourceIDs[NGP_MAX_LAYERS]{},rawRGBNonblack=0,rawWordsNonzero=0;
    uint64_t rawColourHash=0,rawAlphaHash=0;
    bool diagnosticPending=false;
    bool prepared=false;
};
Handoff g;
bool allowed() {char who[32]={};proc_selfname(who,sizeof who);return !strcmp(who,"backboardd");}
void dropViews() {for(auto &m:g.views)if(m) {m->release();m=nullptr;}}
/* Preparation must precede the frame that creating the device permits
 * backboardd to send. CPU stores and console muting finish under this gate. */
void activate() {
    if(g.prepared && !g.gate.armed && vermeer_console_muted && g.gate.state==K70_FRAME_IDLE) {
        g.prepared=false;
        g.gate.armed=1;
        K70BootLog("K70 FRAME: GPU scanout armed by backboardd\n");
        IOLockWakeup(g.lock,&g.gate,false);
    }
}
IOReturn arm(task_t task,const NGPArm &a) {
    if(a.version!=NGP_VERSION || a.reserved || g.gate.state!=K70_FRAME_IDLE ||
       (g.owner && g.owner!=task))return kIOReturnBadArgument;
    /* Prepare the normal single-layer case. Extra layers are allocated only
     * before an unclaimed offer; four unused XR planes would waste 41 MiB. */
    const size_t bytes=kSnapshotBytes;
    for(unsigned i=0;i<1;++i)if(!g.snapshot[i]) {
        g.snapshot[i]=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut|kIOMemoryKernelUserShared,bytes,PAGE_SIZE);
        if(!g.snapshot[i])return kIOReturnNoMemory;
        bzero(g.snapshot[i]->getBytesNoCopy(),bytes);
    }
    if(!g.owner) {task_reference(task);g.owner=task;}
    g.prepared=true;
    K70BootLog("K70 FRAME: GPU presenter prepared console_muted %u\n",vermeer_console_muted?1u:0u);
    activate();
    return kIOReturnSuccess;
}
unsigned panelWord(unsigned x,unsigned y) {
    const volatile uint8_t *p=g.pixels+size_t(y)*5760+size_t(x)*4;
    return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24);
}
void expectPixels() {
    g.diagnosticPending=g.diagnostics<16 && (g.gate.sequence==1 || g.gate.sequence%120==0);
    if(!g.diagnosticPending)return;
    g.alphaMin=255;g.alphaMax=0;g.format=g.frame.layers[0].format;
    g.rawRGBNonblack=0;g.rawWordsNonzero=0;
    g.rawColourHash=14695981039346656037ull;g.rawAlphaHash=g.rawColourHash;
    for(unsigned j=0;j<16;++j)for(unsigned i=0;i<32;++i) {
        const unsigned x=(2*i+1)*1440/64,y=(2*j+1)*3200/32;
        unsigned expected=panelWord(x,y);bool covered=false;
        for(unsigned layer=0;layer<g.frame.count;++layer) {
            const auto &f=g.frame.layers[layer];
            if(x<f.x0 || x>=f.x1 || y<f.y0 || y>=f.y1)continue;
            const unsigned sx=uint64_t(x)*f.lw/f.pw-f.lx,sy=uint64_t(y)*f.lh/f.ph-f.ly;
            const auto *snapshot=static_cast<const uint8_t *>(g.snapshot[layer]->getBytesNoCopy());
            const unsigned word=k70_pixel_word(snapshot+size_t(sy)*f.stride+size_t(sx)*4);
            if(word)++g.rawWordsNonzero;
            const unsigned rawRGB=f.format==NGP_SOURCE_BGRA?word:k70_xr10_bgra(word,255,0,true);
            if(rawRGB&0xffffffu)++g.rawRGBNonblack;
            for(unsigned c=0;c<4;++c){g.rawColourHash^=(word>>(8*c))&255;g.rawColourHash*=1099511628211ull;}
            if(f.format==NGP_SOURCE_BGRA)expected=word;
            else {
                const unsigned alpha=snapshot[f.alpha_offset+size_t(sy)*f.alpha_stride+sx];
                g.rawAlphaHash^=alpha;g.rawAlphaHash*=1099511628211ull;
                if(alpha<g.alphaMin)g.alphaMin=alpha;if(alpha>g.alphaMax)g.alphaMax=alpha;
                expected=k70_xr10_bgra(word,alpha,expected,!covered);
            }
            covered=true;
        }
        g.expected[j*32+i]=expected;
    }
}
void observePixels() {
    if(!g.diagnosticPending)return;
    g.diagnosticPending=false;++g.diagnostics;
    unsigned nonblack=0,expectedNonblack=0,mismatch=0;
    uint64_t actualHash=14695981039346656037ull,expectedHash=actualHash;
    for(unsigned j=0;j<16;++j)for(unsigned i=0;i<32;++i) {
        const unsigned x=(2*i+1)*1440/64,y=(2*j+1)*3200/32;
        const unsigned actual=panelWord(x,y),expected=g.expected[j*32+i];
        if(actual&0xffffffu)++nonblack;if(expected&0xffffffu)++expectedNonblack;
        if((actual&0xffffffu)!=(expected&0xffffffu))++mismatch;
        for(unsigned c=0;c<3;++c) {
            actualHash^=(actual>>(8*c))&255;actualHash*=1099511628211ull;
            expectedHash^=(expected>>(8*c))&255;expectedHash*=1099511628211ull;
        }
    }
    K70BootLog("K70 PIXEL: sequence %llu fmt %08x RGB nonblack %u expected %u mismatch %u alpha %u..%u hashes %llx/%llx\n",
        (unsigned long long)g.gate.sequence,g.format,nonblack,expectedNonblack,mismatch,g.alphaMin,g.alphaMax,
        (unsigned long long)actualHash,(unsigned long long)expectedHash);
    K70BootLog("K70 SOURCE: sequence %llu ids %u/%u/%u/%u rawRGB %u nonzeroWords %u colourHash %llx alphaHash %llx\n",
        (unsigned long long)g.gate.sequence,g.sourceIDs[0],g.sourceIDs[1],g.sourceIDs[2],g.sourceIDs[3],
        g.rawRGBNonblack,g.rawWordsNonzero,(unsigned long long)g.rawColourHash,(unsigned long long)g.rawAlphaHash);
}
bool scanoutMapping(uintptr_t va,uint64_t pa,size_t bytes) {
#if defined(K70_FRAME_HOST_TEST)
    return va && pa==0xb8000000ull && bytes==18432000;
#else
    uint64_t first=0,last=0;
    disable_preemption();const boolean_t interrupts=ml_set_interrupts_enabled(FALSE);
    const uintptr_t lastVA=va+bytes-1;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(first):"r"(va):"memory");
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(last):"r"(lastVA):"memory");
    ml_set_interrupts_enabled(interrupts);enable_preemption();
    const bool valid=!(first&1) && !(last&1) && first>>56==0x44 && last>>56==0x44 &&
        (first&0x0000fffffffff000ull)==(pa&~4095ull) &&
        (last&0x0000fffffffff000ull)==((pa+bytes-1)&~4095ull);
    K70BootLog("K70 PIXEL: scanout WC PAR %llx/%llx PA %llx..%llx valid %u\n",
        (unsigned long long)first,(unsigned long long)last,(unsigned long long)pa,
        (unsigned long long)(pa+bytes-1),valid?1u:0u);
    return valid;
#endif
}
}
extern "C" IOReturn k70_frame_register(IOMemoryDescriptor *memory,uint64_t pa,uint32_t w,uint32_t h,uint32_t stride,uintptr_t pixels)
{
    if(g.lock || !memory || pa!=UINT64_C(0xb8000000) || w!=1440 || h!=3200 || stride!=5760 ||
       memory->getLength()!=uint64_t(stride)*h || memory->getLength()%PAGE_SIZE || !pixels ||
       memory->getLength()>UINTPTR_MAX-pixels)return kIOReturnBadArgument;
    if(!scanoutMapping(pixels,pa,memory->getLength()))return kIOReturnUnsupported;
    g.pixels=reinterpret_cast<volatile uint8_t *>(pixels);
    g.lock=IOLockAlloc();if(!g.lock)return kIOReturnNoMemory;
    memory->retain();g.scanout=memory;g.mode={NGP_VERSION,NGP_BGRX8888,w,h,stride,memory->getLength()};
    K70BootLog("K70 FRAME: scanout registered %ux%u stride %u bytes %llu\n",w,h,stride,(unsigned long long)memory->getLength());
    return kIOReturnSuccess;
}
extern "C" unsigned k70_frame_offer(const K70FrameInput *input,unsigned count)
{
    if(!g.lock)return K70_FRAME_CPU;
    IOLockLock(g.lock);
    unsigned route=k70_frame_route(&g.gate);
    if(route!=K70_FRAME_GPU) {IOLockUnlock(g.lock);return route;}
    if(!input || !count || count>NGP_MAX_LAYERS || g.gate.sequence==UINT64_MAX) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
    uint32_t widths[NGP_MAX_LAYERS]{},heights[NGP_MAX_LAYERS]{};
    uint64_t alphaOrigins[NGP_MAX_LAYERS]{};
    for(unsigned i=0;i<count;++i) {
        const auto &f=input[i];const auto &p=f.scale;const auto &layout=f.layout;
        const uint32_t stride=k70_pixel_color_stride(layout);
        const uint64_t offset=k70_pixel_color_offset(layout);
        if(!f.base || !k70_pixel_layout_valid(layout) || layout.bytes>UINTPTR_MAX-(uintptr_t)f.base ||
           p.lw!=1125 || p.lh!=2436 || p.pw!=1440 || p.ph!=3200 ||
           p.x0>=p.x1 || p.y0>=p.y1 || p.x1>p.pw || p.y1>p.ph || p.lx>(uint64_t)p.x0*p.lw/p.pw ||
           p.ly>(uint64_t)p.y0*p.lh/p.ph || p.src<offset || p.src>layout.bytes || (p.src&3)) {
            IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;
        }
        widths[i]=vframe_scale_x(&p,p.x1-1)+1;heights[i]=vframe_scale_y(&p,p.y1-1)+1;
        const uint64_t row=uint64_t(widths[i])*4,last=(uint64_t(heights[i])-1)*stride;
        const uint64_t crop=p.src-offset,cropY=crop/stride,cropX=(crop%stride)/4;
        if(widths[i]>1125 || heights[i]>2436 || cropX+widths[i]>layout.width || cropY+heights[i]>layout.height ||
           row>stride || last>layout.bytes-p.src || row>layout.bytes-p.src-last) {
            IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;
        }
        const uint64_t colorBytes=row*heights[i],alphaOffset=(colorBytes+15)&~uint64_t(15);
        const uint64_t required=layout.format==K70_PIXEL_B3A8?alphaOffset+uint64_t(widths[i])*heights[i]:colorBytes;
        if(required>kSnapshotBytes || (g.snapshot[i] && required>g.snapshot[i]->getLength())) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
        if(layout.format==K70_PIXEL_B3A8) {
            const auto &alpha=layout.plane[1];
            alphaOrigins[i]=alpha.offset+cropY*alpha.stride+cropX;
            const uint64_t tail=(uint64_t(heights[i])-1)*alpha.stride+widths[i];
            if(alphaOrigins[i]>layout.bytes || tail>layout.bytes-alphaOrigins[i]) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
        }
    }
    for(unsigned i=0;i<count;++i)if(!g.snapshot[i]) {
        g.snapshot[i]=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut|kIOMemoryKernelUserShared,kSnapshotBytes,PAGE_SIZE);
        if(!g.snapshot[i]) {
            if(g.snapshotRefusals++<4)K70BootLog("K70 PIXEL: snapshot layer %u allocation refused; prior queued frame retained\n",i);
            IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;
        }
        bzero(g.snapshot[i]->getBytesNoCopy(),kSnapshotBytes);
    }
    NGPScaleSwap next{};next.version=NGP_VERSION;next.count=count;
    for(unsigned i=0;i<NGP_MAX_LAYERS;++i)g.sourceIDs[i]=i<count?input[i].surfaceID:0;
    for(unsigned i=0;i<count;++i) {
        const auto &f=input[i];const auto &p=f.scale;const auto &layout=f.layout;
        const uint32_t stride=widths[i]*4;const uint64_t colorBytes=uint64_t(stride)*heights[i];
        auto *dest=static_cast<uint8_t *>(g.snapshot[i]->getBytesNoCopy());
        for(uint32_t y=0;y<heights[i];++y)
            memcpy(dest+size_t(y)*stride,f.base+p.src+uint64_t(y)*k70_pixel_color_stride(layout),stride);
        const bool xr=layout.format==K70_PIXEL_B3A8;
        const uint64_t alphaOffset=xr?(colorBytes+15)&~uint64_t(15):0,alphaBytes=xr?uint64_t(widths[i])*heights[i]:0;
        if(xr)for(uint32_t y=0;y<heights[i];++y)
            memcpy(dest+alphaOffset+size_t(y)*widths[i],f.base+alphaOrigins[i]+uint64_t(y)*layout.plane[1].stride,widths[i]);
        next.layers[i]={0,g.snapshot[i]->getLength(),colorBytes,alphaOffset,alphaBytes,stride,xr?widths[i]:0,layout.format,
            p.lx,p.ly,p.x0,p.y0,p.x1,p.y1,p.lw,p.lh,p.pw,p.ph,0};
        if(g.gate.sequence==0)K70BootLog("K70 PIXEL: input layer %u fmt %08x planes %u color %llu stride %u alpha %llu stride %u crop %ux%u\n",
            i,layout.format,layout.plane_count,(unsigned long long)k70_pixel_color_offset(layout),k70_pixel_color_stride(layout),
            (unsigned long long)(xr?layout.plane[1].offset:0),xr?layout.plane[1].stride:0,widths[i],heights[i]);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if(!k70_frame_queue(&g.gate)) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
    next.sequence=g.gate.sequence;g.frame=next;IOLockWakeup(g.lock,&g.gate,false);
    IOLockUnlock(g.lock);return K70_FRAME_GPU;
}
extern "C" bool k70_frame_cpu_begin(void)
{
    if(!g.lock)return false;
    IOLockLock(g.lock);
    if(k70_frame_route(&g.gate)!=K70_FRAME_CPU) {IOLockUnlock(g.lock);return false;}
    return true;
}
extern "C" void k70_frame_cpu_end(void)
{
    /* Publish panel stores before ARM or another owner can proceed. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    activate();
    IOLockUnlock(g.lock);
}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void)
{
    if(!g.lock || !allowed())return nullptr;
    /* Borrowed: clientMemoryForType retains its returned descriptor. */
    return g.scanout;
}
extern "C" IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inBytes,void *out,size_t outBytes)
{
    if(!g.lock || !allowed() || !task)return kIOReturnNotReady;
    IOLockLock(g.lock);IOReturn status=kIOReturnBadArgument;
    if(op==NGP_METHOD_MODE && !inBytes && out && outBytes==sizeof(NGPMode)) {memcpy(out,&g.mode,sizeof g.mode);status=kIOReturnSuccess;}
    else if(op==NGP_METHOD_ARM && in && inBytes==sizeof(NGPArm) && !outBytes) {
        NGPArm request{};memcpy(&request,in,sizeof request);status=arm(task,request);
    } else if(op==NGP_METHOD_WAIT_SWAP && task==g.owner && in && inBytes==sizeof(NGPWait) && out && outBytes==sizeof(NGPScaleSwap)) {
        NGPWait wait{};memcpy(&wait,in,sizeof wait);
        if(wait.timeout_ms && wait.timeout_ms<=1000 && g.gate.state!=K70_FRAME_QUARANTINED) {
            uint64_t deadline=0;clock_interval_to_deadline((uint32_t)wait.timeout_ms,kMillisecondScale,&deadline);
            while((g.prepared || g.gate.armed) && g.gate.state==K70_FRAME_IDLE) {
                if(IOLockSleepDeadline(g.lock,&g.gate,deadline,THREAD_UNINT)==THREAD_TIMED_OUT)break;
            }
            status=kIOReturnTimeout;
            if(g.gate.state==K70_FRAME_QUEUED) {
                bool mapped=true;
                for(unsigned i=0;i<g.frame.count;++i) {
                    g.views[i]=g.snapshot[i]->createMappingInTask(task,0,kIOMapAnywhere|kIOMapReadOnly);
                    if(!g.views[i] || !g.views[i]->getVirtualAddress() || g.views[i]->getLength()<g.frame.layers[i].allocation_bytes) {mapped=false;break;}
                    g.frame.layers[i].address=g.views[i]->getVirtualAddress();
                }
                if(mapped && k70_frame_claim(&g.gate)) {expectPixels();memcpy(out,&g.frame,sizeof g.frame);status=kIOReturnSuccess;}
                else {dropViews();g.gate.armed=0;g.gate.state=K70_FRAME_IDLE;status=kIOReturnNoMemory;}
            }
        }
    } else if(op==NGP_METHOD_COMPLETE_SWAP && task==g.owner && in && inBytes==sizeof(NGPComplete) && !outBytes) {
        NGPComplete done{};memcpy(&done,in,sizeof done);
        if(k70_frame_complete(&g.gate,done.sequence,done.status,done.flags)) {
            if(g.gate.state==K70_FRAME_IDLE)dropViews();
            if(!done.status && (done.flags&NGP_QUIESCENT)) {
                ++g.completed;observePixels();
                if(g.completed==1 || g.completed%120==0)
                    K70BootLog("K70 FRAME: GPU completed %llu sequence %llu layers %u\n",
                        (unsigned long long)g.completed,(unsigned long long)done.sequence,g.frame.count);
            } else {
                K70BootLog("K70 FRAME: GPU completion failed sequence %llu status 0x%x flags 0x%x state %u\n",
                    (unsigned long long)done.sequence,done.status,done.flags,g.gate.state);
            }
            status=kIOReturnSuccess;
        }
    }
    if(op==NGP_METHOD_MODE || op==NGP_METHOD_ARM || (status && status!=kIOReturnTimeout))
        K70BootLog("K70 FRAME: handoff method %u status 0x%x prepared %u armed %u state %u\n",op,status,g.prepared?1u:0u,g.gate.armed,g.gate.state);
    IOLockUnlock(g.lock);return status;
}
