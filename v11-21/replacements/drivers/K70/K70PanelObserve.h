/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include "K70BootLog.h"
#include "K70AutorefreshFix.h"

/* DPU9 observation and the measured test181 count correction.
 * Called on the framebuffer workloop, once per two seconds.
 * The exact boot framebuffer is required before touching MDSS, and major 9
 * is required before mapping interfaces. Samples show raw command/TE state;
 * a timer callback is not evidence of a hardware refresh. */
class K70PanelObserve {
    IOMemoryMap *top_,*interfaces_;
    uint64_t last_;
    uint32_t frames_[2],lines_,reported_;
    bool disabled_,previous_;
    bool repairConfigured_,repairEnabled_,repairAttempted_,repairOwned_;
    uint32_t repairBefore_;
    static uint32_t read(IOMemoryMap *m,unsigned offset) {
        return *reinterpret_cast<volatile const uint32_t *>(m->getVirtualAddress()+offset);
    }
    static void drop(IOMemoryMap *&m) {if(m){m->unmap();m->release();m=nullptr;}}
    static void write(IOMemoryMap *m,unsigned offset,uint32_t value) {
        *reinterpret_cast<volatile uint32_t *>(m->getVirtualAddress()+offset)=value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    static IOMemoryMap *map(uint64_t address,unsigned bytes,bool writable=false) {
        IODeviceMemory *memory=IODeviceMemory::withRange(address,bytes);
        IOMemoryMap *result=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|(writable?0:kIOMapReadOnly)):nullptr;
        if(memory)memory->release();
        if(result && (!result->getVirtualAddress() || result->getPhysicalAddress()!=address || result->getLength()<bytes))drop(result);
        return result;
    }
public:
    /* AppleCLCD's IOKit allocation zeroes these fields. Workloop owns access. */
    void release() {
        if(repairOwned_ && interfaces_ && read(interfaces_,0x2b4)==0x80000001u) {
            write(interfaces_,0x2b4,repairBefore_);
            K70BootLog("K70 DPU: restore autorefresh %08x readback %08x\n",
                repairBefore_,read(interfaces_,0x2b4));
        }
        repairOwned_=false;drop(interfaces_);drop(top_);disabled_=true;
    }
    void sample(uint64_t physical,unsigned width,unsigned height,unsigned stride) {
        if(disabled_ || lines_>=900)return;
        if(physical!=0xb8000000ull || width!=1440 || height!=3200 || stride!=5760){disabled_=true;return;}
        if(!repairConfigured_) {
            uint32_t option=0;
            PE_parse_boot_argn("k70_dpu_fix",&option,sizeof option);
            repairEnabled_=option==1;repairConfigured_=true;
        }
        const uint64_t now=mach_absolute_time();uint64_t elapsed=0;
        if(last_){absolutetime_to_nanoseconds(now-last_,&elapsed);if(elapsed<2000000000ull)return;}
        if(!top_)top_=map(0x0ae00000,0x2000);
        if(!top_){disabled_=true;K70BootLog("K70 DPU: map-top refused\n");return;}
        const uint32_t version=read(top_,0x1000);
        if((version>>28)!=9){K70BootLog("K70 DPU: version %08x expected major9; stopped\n",version);release();return;}
        if(!interfaces_)interfaces_=map(0x0ae36000,0x2000,repairEnabled_);
        if(!interfaces_){K70BootLog("K70 DPU: map-interface refused\n");release();return;}
        if(repairEnabled_ && !repairAttempted_) {
            repairAttempted_=true;
            const uint32_t before=read(interfaces_,0x2b4);
            const bool eligible=k70_autorefresh_eligible(version,width,height,
                read(interfaces_,0x26c),read(interfaces_,0),read(interfaces_,0xa8),
                read(interfaces_,0x84),read(interfaces_,0x284),before,
                read(interfaces_,0x1284),read(interfaces_,0x12b4));
            uint32_t after=before;
            if(eligible) {
                repairBefore_=before;
                write(interfaces_,0x2b4,0x80000001u);
                after=read(interfaces_,0x2b4);repairOwned_=after==0x80000001u;
            }
            K70BootLog("K70 DPU: autorefresh eligible %u before %08x after %08x applied %u\n",
                eligible?1u:0u,before,after,repairOwned_?1u:0u);
        }
        /* E7 measured two full lines every two seconds (~736 per capture).
         * Emit one combined line per 60 hardware frames (~1s at 60Hz) instead;
         * sampled status fields still expose a stuck autorefresh/TE state. */
        const unsigned b0=0,b1=0x1000;
        const uint32_t frame0=read(interfaces_,b0+0xac),frame1=read(interfaces_,b1+0xac);
        const uint32_t delta0=frame0-frames_[0],delta1=frame1-frames_[1];
        const bool emit=!previous_ || frame0-reported_>=60u || frame1-reported_>=60u;
        frames_[0]=frame0;frames_[1]=frame1;
        if(emit) {
            reported_=frame0;
            K70BootLog("K70 DPU: dt_us %llu v %08x frame %u/%u delta %u/%u status %x/%x timing %x/%x count %x/%x cmd %x/%x te %x/%x ar %x/%x line %u/%u tl %x/%x\n",
                (unsigned long long)(elapsed/1000),version,frame0,frame1,delta0,delta1,
                read(interfaces_,b0+0x26c),read(interfaces_,b1+0x26c),
                read(interfaces_,b0),read(interfaces_,b1),
                read(interfaces_,b0+0xa8),read(interfaces_,b1+0xa8),
                read(interfaces_,b0+0x84),read(interfaces_,b1+0x84),
                read(interfaces_,b0+0x284),read(interfaces_,b1+0x284),
                read(interfaces_,b0+0x2b4),read(interfaces_,b1+0x2b4),
                read(interfaces_,b0+0xb0),read(interfaces_,b1+0xb0),
                read(interfaces_,b0+0x2b0),read(interfaces_,b1+0x2b0));
        }
        ++lines_;previous_=true;last_=now;
    }
};
