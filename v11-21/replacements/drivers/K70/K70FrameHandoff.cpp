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
#include <string.h>
extern "C" void proc_selfname(char *,int);
extern "C" int vermeer_console_muted;
namespace {
struct Handoff {
    IOLock *lock=nullptr;
    IOMemoryDescriptor *scanout=nullptr;
    IOBufferMemoryDescriptor *snapshot[NGP_MAX_LAYERS]{};
    IOMemoryMap *views[NGP_MAX_LAYERS]{};
    task_t owner=TASK_NULL;
    NGPMode mode{};NGPScaleSwap frame{};K70FrameGate gate{};
    uint64_t completed=0;
};
Handoff g;
bool allowed() {char who[32]={};proc_selfname(who,sizeof who);return !strcmp(who,"backboardd");}
void dropViews() {for(auto &m:g.views)if(m) {m->release();m=nullptr;}}
IOReturn arm(task_t task,const NGPArm &a) {
    if(a.version!=NGP_VERSION || a.reserved || !vermeer_console_muted || g.gate.state!=K70_FRAME_IDLE ||
       (g.owner && g.owner!=task))return kIOReturnBadArgument;
    constexpr size_t bytes=(size_t(1125)*2436*4+16383)&~size_t(16383);
    for(unsigned i=0;i<NGP_MAX_LAYERS;++i)if(!g.snapshot[i]) {
        g.snapshot[i]=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut|kIOMemoryKernelUserShared,bytes,PAGE_SIZE);
        if(!g.snapshot[i])return kIOReturnNoMemory;
        bzero(g.snapshot[i]->getBytesNoCopy(),bytes);
    }
    if(!g.owner) {task_reference(task);g.owner=task;}
    g.gate.armed=1;
    K70BootLog("K70 FRAME: GPU scanout armed by backboardd\n");
    return kIOReturnSuccess;
}
}
extern "C" IOReturn k70_frame_register(IOMemoryDescriptor *memory,uint64_t pa,uint32_t w,uint32_t h,uint32_t stride)
{
    if(g.lock || !memory || pa!=UINT64_C(0xb8000000) || w!=1440 || h!=3200 || stride!=5760 ||
       memory->getLength()<uint64_t(stride)*h || memory->getLength()%PAGE_SIZE)return kIOReturnBadArgument;
    g.lock=IOLockAlloc();if(!g.lock)return kIOReturnNoMemory;
    memory->retain();g.scanout=memory;g.mode={NGP_VERSION,NGP_BGRX8888,w,h,stride,memory->getLength()};
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
    for(unsigned i=0;i<count;++i) {
        const auto &f=input[i];const auto &p=f.scale;
        if(!f.base || f.bytes>UINTPTR_MAX-(uintptr_t)f.base || !g.snapshot[i] || !f.stride || (f.stride&3) ||
           p.lw!=1125 || p.lh!=2436 || p.pw!=1440 || p.ph!=3200 ||
           p.x0>=p.x1 || p.y0>=p.y1 || p.x1>p.pw || p.y1>p.ph || p.lx>(uint64_t)p.x0*p.lw/p.pw ||
           p.ly>(uint64_t)p.y0*p.lh/p.ph || p.src>f.bytes) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
        widths[i]=vframe_scale_x(&p,p.x1-1)+1;heights[i]=vframe_scale_y(&p,p.y1-1)+1;
        const uint64_t row=uint64_t(widths[i])*4,last=(uint64_t(heights[i])-1)*f.stride;
        if(widths[i]>1125 || heights[i]>2436 || row>f.stride || last>f.bytes-p.src ||
           row>f.bytes-p.src-last || row*heights[i]>g.snapshot[i]->getLength()) {IOLockUnlock(g.lock);return K70_FRAME_BLOCKED;}
    }
    /* No claimed GPU frame can enter this branch. Replacing an unclaimed
     * queued frame is safe. Snapshot only visible source rows, without CPU
     * scaling, so SwapEnd/SwapWait cannot recycle memory beneath a GPU read. */
    NGPScaleSwap next{};next.version=NGP_VERSION;next.count=count;
    for(unsigned i=0;i<count;++i) {
        const auto &f=input[i];const auto &p=f.scale;const uint32_t stride=widths[i]*4;
        auto *dest=static_cast<uint8_t *>(g.snapshot[i]->getBytesNoCopy());
        for(uint32_t y=0;y<heights[i];++y)memcpy(dest+size_t(y)*stride,f.base+p.src+uint64_t(y)*f.stride,stride);
        next.layers[i]={0,g.snapshot[i]->getLength(),uint64_t(stride)*heights[i],stride,
            p.lx,p.ly,p.x0,p.y0,p.x1,p.y1,p.lw,p.lh,p.pw,p.ph,0};
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
            while(g.gate.armed && g.gate.state==K70_FRAME_IDLE) {
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
                if(mapped && k70_frame_claim(&g.gate)) {memcpy(out,&g.frame,sizeof g.frame);status=kIOReturnSuccess;}
                else {dropViews();g.gate.armed=0;g.gate.state=K70_FRAME_IDLE;status=kIOReturnNoMemory;}
            }
        }
    } else if(op==NGP_METHOD_COMPLETE_SWAP && task==g.owner && in && inBytes==sizeof(NGPComplete) && !outBytes) {
        NGPComplete done{};memcpy(&done,in,sizeof done);
        if(k70_frame_complete(&g.gate,done.sequence,done.status,done.flags)) {
            if(g.gate.state==K70_FRAME_IDLE)dropViews();
            if(!done.status && (done.flags&NGP_QUIESCENT)) {
                ++g.completed;
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
    IOLockUnlock(g.lock);return status;
}
