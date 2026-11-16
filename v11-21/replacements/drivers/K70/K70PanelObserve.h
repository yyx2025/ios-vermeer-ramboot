/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "K70BootLog.h"

/* Read-only DPU9 observation, adapted from the retained K70 test181 source.
 * Called on the framebuffer workloop, once per two seconds. No MMIO writes.
 * The exact boot framebuffer is required before touching MDSS, and major 9
 * is required before mapping interfaces. Samples show raw command/TE state;
 * a timer callback is not evidence of a hardware refresh. */
class K70PanelObserve {
    IOMemoryMap *top_,*interfaces_;
    uint64_t last_;
    uint32_t frames_[2],lines_;
    bool disabled_,previous_;
    static uint32_t read(IOMemoryMap *m,unsigned offset) {
        return *reinterpret_cast<volatile const uint32_t *>(m->getVirtualAddress()+offset);
    }
    static void drop(IOMemoryMap *&m) {if(m){m->unmap();m->release();m=nullptr;}}
    static IOMemoryMap *map(uint64_t address,unsigned bytes) {
        IODeviceMemory *memory=IODeviceMemory::withRange(address,bytes);
        IOMemoryMap *result=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|kIOMapReadOnly):nullptr;
        if(memory)memory->release();
        if(result && (!result->getVirtualAddress() || result->getPhysicalAddress()!=address || result->getLength()<bytes))drop(result);
        return result;
    }
public:
    /* AppleCLCD's IOKit allocation zeroes these fields. Workloop owns access. */
    void release() {drop(interfaces_);drop(top_);disabled_=true;}
    void sample(uint64_t physical,unsigned width,unsigned height,unsigned stride) {
        if(disabled_ || lines_>=900)return;
        if(physical!=0xb8000000ull || width!=1440 || height!=3200 || stride!=5760){disabled_=true;return;}
        const uint64_t now=mach_absolute_time();uint64_t elapsed=0;
        if(last_){absolutetime_to_nanoseconds(now-last_,&elapsed);if(elapsed<2000000000ull)return;}
        if(!top_)top_=map(0x0ae00000,0x2000);
        if(!top_){disabled_=true;K70BootLog("K70 DPU: map-top refused\n");return;}
        const uint32_t version=read(top_,0x1000);
        if((version>>28)!=9){K70BootLog("K70 DPU: version %08x expected major9; stopped\n",version);release();return;}
        if(!interfaces_)interfaces_=map(0x0ae36000,0x2000);
        if(!interfaces_){K70BootLog("K70 DPU: map-interface refused\n");release();return;}
        for(unsigned i=0;i<2;++i) {
            const unsigned b=i*0x1000;const uint32_t frame=read(interfaces_,b+0xac);
            K70BootLog("K70 DPU: dt_us %llu v %08x intf %u frame %u delta %u previous %u status %x timing %x count %x cmd %x te %x ar %x line %u tl %x\n",
                (unsigned long long)(elapsed/1000),version,i+1,frame,frame-frames_[i],previous_?1u:0u,
                read(interfaces_,b+0x26c),read(interfaces_,b),read(interfaces_,b+0xa8),read(interfaces_,b+0x84),
                read(interfaces_,b+0x284),read(interfaces_,b+0x2b4),read(interfaces_,b+0xb0),read(interfaces_,b+0x2b0));
            frames_[i]=frame;
        }
        ++lines_;previous_=true;last_=now;
    }
};
