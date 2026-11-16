#include "K70BootLog.h"
/* SPDX-License-Identifier: MIT */
/* K70 wiring of the shared SM8550 GMU implementation. Constants are verified
 * against the retained original K70 DT and the pinned mainline policy. See
 * startup-inputs.json. Firmware bytes stay local to image assembly. */
#include "K70GpuStartup.h"
#include "GpuPowerBoard.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoGmuPlatform.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoFirmware.h"
#include "K70AdrenoFirmwareRules.h"
#include "K70GpuZap.h"
#include "K70GpuCp.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoA740HwInit.h"
#include "AdrenoPM4.h"
#include "ArmSmmu4KMmio.h"
#include "ShengGmuCommandDb.h"
#include "ShengGmuSource.h"
#include "ShengGmuVotesSource.h"
#include "ShengGmuCommandDbSource.h"
#include "ShengGpuSmmuSource.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <string.h>
#include <mach/vm_param.h>
#include <vm/pmap.h>
#include <kern/clock.h>

extern "C" const uint8_t native_adreno_firmware_image[];
extern "C" const uint64_t native_adreno_firmware_image_size;
extern "C" const uint8_t native_adreno_sqe_firmware_image[],native_adreno_zap_image[];
extern "C" const uint64_t native_adreno_sqe_firmware_image_size,native_adreno_zap_image_size;
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern "C" void ml_io_unmap(vm_offset_t,vm_size_t);
extern "C" uint64_t gPhysBase,gPhysSize;
/* Exported by the target XNU8796 ARM pmap implementation; omitted from its
 * legacy umbrella header. Signature verified against arm/pmap/pmap.h. */
extern "C" boolean_t pmap_valid_page(ppnum_t);

namespace {
class K70GpuStartup final {
    OSSharedPtr<IOMemoryMap> power_maps_[3], gmu_maps_[ShengGmuWindows], smmu_map_, db_map_;
    volatile uint8_t *power_[3]{};
    NativeAdrenoGmuContext gmu_context_;
    NativeAdrenoGpuContext gpu_context_;
    NativeAdrenoGmuPlatform platform_;
    ArmSmmu4KMmio smmu_mmio_{};
    ArmSmmu4KIo smmu_io_{};
    ShengGmuDevice device_{};
    ShengGmuConfig firmware_config_{};
    NativeAdrenoGmuTime time_{};
    NativeAdrenoRing ring_;
    NativeAdrenoCpInit cp_;
    NativeAdrenoSubmit submit_;
    NativeAdrenoTimer timer_;
    NativeAdrenoMemory zap_metadata_;
    NativeAdrenoFirmwareView sqe_{},zap_{};
    K70ZapPlan zap_plan_{};
    K70ScmCall scm_{};
    NativeAdrenoRingIo ring_io_{};
    NativeAdrenoRingClock ring_clock_{};
    vm_offset_t zap_mapping_=0;
    vm_size_t zap_mapping_bytes_=0;
    AdrenoHfiStartupConfig startup_{};
    ShengGmuStartupResult startup_result_{};
    alignas(8) uint8_t command_db_[SHENG_GMU_COMMAND_DB_BYTES]{};
    K70GpuStartupReport report_{};
    bool gx_held_=false;
    static constexpr uint64_t power_phys_[3]={GPB_GCC_GPU_PAGE,GPB_GCC_VOTE_PAGE,GPB_GPUCC_BASE};
    static constexpr size_t power_bytes_[3]={0x1000,0x1000,GPB_GPUCC_SIZE};
    static constexpr size_t coreAt(uint32_t word) {return size_t(word)*4-SHENG_GMU_CORE_GPU_DELTA;}
    static bool clockWrite(unsigned w,uint32_t at) {
        if(w==GPB_WIN_GCC_VOTE)return at==GPB_GCC_PLL_VOTE;
        if(w==GPB_WIN_GCC_GPU)return at==GPB_GCC_GPU_CFG_AHB ||
            at==GPB_GCC_DDRSS_GPU_AXI || at==GPB_GCC_GPU_MEMNOC_GFX;
        if(w!=GPB_WIN_GPUCC)return false;
        return at<GPB_CC_PLL1+0x40 || at==GPB_CC_CX_GDSCR || at==GPB_CC_AHB_CLK ||
            at==GPB_CC_SMMU_VOTE_CLK || at==GPB_CC_GMU_RCGR || at==GPB_CC_GMU_RCGR+4 ||
            at==GPB_CC_HUB_RCGR || at==GPB_CC_HUB_RCGR+4 || at==GPB_CC_CXO_AON_CLK ||
            at==GPB_CC_DEMET_CLK || at==GPB_CC_CX_GMU_CLK || at==GPB_CC_CXO_CLK ||
            at==GPB_CC_HUB_CX_INT_CLK;
    }
    static int powerRead(void *cookie,unsigned window,uint32_t at,uint32_t *out) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        if(!out || window>=3 || !s->power_[window] || (at&3) ||
            at>=power_bytes_[window])return 1;
        *out=*reinterpret_cast<volatile uint32_t *>(s->power_[window]+at);return 0;
    }
    static int powerWrite(void *cookie,unsigned window,uint32_t at,uint32_t value) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        if(window>=3 || !s->power_[window] || (at&3) || at>=power_bytes_[window] ||
            !clockWrite(window,at))return 1;
        *reinterpret_cast<volatile uint32_t *>(s->power_[window]+at)=value;
        NativeAdrenoPublishBarrier();return 0;
    }
    static void delay(void *,unsigned us) {IODelay(us);}
    static int hfiWait(void *) {IODelay(100);return 0;}
    static int gpuRead(void *cookie,uint32_t reg,uint32_t *out) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        if(!s || !s->gx_held_ || reg>=sheng_gmu_window_bytes[ShengGmuGpu]/4)return 1;
        return s->device_.io.read32(s->device_.io.cookie,ShengGmuGpu,size_t(reg)*4,out);
    }
    static int gpuWrite(void *cookie,uint32_t reg,uint32_t value) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        if(!s || !s->gx_held_ || reg>=sheng_gmu_window_bytes[ShengGmuGpu]/4)return 1;
        return s->device_.io.write32(s->device_.io.cookie,ShengGmuGpu,size_t(reg)*4,value);
    }
    static int gmuWrite(void *cookie,uint32_t reg,uint32_t value) {
        auto *s=static_cast<K70GpuStartup *>(cookie);return s?s->writeCore(reg,value):1;
    }
    static int gpuBarrier(void *) {NativeAdrenoPublishBarrier();return 0;}
    static int ringIdle(void *) {IODelay(10);return 0;}
    static uint64_t ringNow(void *) {
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
    }
    static int ringPublish(void *cookie,const volatile uint32_t *words,size_t bytes) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        auto &m=s->gpu_context_.buffer(1);
        if(words!=m.mapping() || bytes!=m.bytes())return 1;
        return m.publishForDevice(0,bytes,false)!=kIOReturnSuccess;
    }
    static int ringAcquire(void *cookie,volatile uint32_t *words,size_t bytes) {
        auto *s=static_cast<K70GpuStartup *>(cookie);
        auto &m=s->gpu_context_.buffer(2);
        if(words!=m.mapping() || bytes!=m.bytes())return 1;
        return m.acquireDeviceWrites(0,bytes)!=kIOReturnSuccess;
    }
    IOReturn gpuWrite64(uint32_t reg,uint64_t value) {
        return gpuWrite(this,reg,uint32_t(value)) || gpuWrite(this,reg+1,uint32_t(value>>32))?
            kIOReturnIOError:kIOReturnSuccess;
    }
    IOReturn setGpuActlr() {
        report_.stage=K70GpuSmmuActlr;
        uint32_t id1=0;
        if(smmu_io_.read32(smmu_io_.cookie,0x24,&id1))return stop(kIOReturnIOError);
        const size_t page=(id1>>31)?0x10000:0x1000,bank0=page<<(((id1>>28)&7)+1);
        if(bank0+0x800>SHENG_GPU_SMMU_BYTES)return stop(kIOReturnUnsupported);
        if(smmu_io_.write32(smmu_io_.cookie,bank0+4,0x301) ||
           smmu_io_.barrier(smmu_io_.cookie) ||
           smmu_io_.write32(smmu_io_.cookie,bank0+0x618,0) ||
           smmu_io_.write32(smmu_io_.cookie,bank0+0x7f0,0) ||
           smmu_io_.barrier(smmu_io_.cookie))return stop(kIOReturnIOError);
        for(unsigned i=0;i<1000;++i) {
            uint32_t busy=0;
            if(smmu_io_.read32(smmu_io_.cookie,bank0+0x7f4,&busy))return stop(kIOReturnIOError);
            if(!(busy&1)) {
                if(smmu_io_.read32(smmu_io_.cookie,bank0+4,&report_.actlr))return stop(kIOReturnIOError);
                return report_.actlr==0x301?kIOReturnSuccess:stop(kIOReturnIOError);
            }
            IODelay(1);
        }
        return stop(kIOReturnTimeout);
    }
    IOReturn waitFence(uint32_t fence) {
        report_.submitted_fence=fence;
        auto result=ring_.wait(fence,&ring_clock_,200000,&report_.observed_fence);
        return result==NativeAdrenoRingFenceComplete?kIOReturnSuccess:
            stop(result==NativeAdrenoRingFenceTimeout?kIOReturnTimeout:kIOReturnIOError);
    }
    IOReturn scmCall(unsigned cmd,uint64_t pa=0,uint64_t bytes=0) {
        K70BootLog("K70 GPU: SCM begin command %u pa=0x%llx bytes=%llu\n",cmd,
            (unsigned long long)pa,(unsigned long long)bytes);
        const uint64_t began=ringNow(nullptr)/1000;
        if(!k70_scm_begin(&scm_,cmd,pa,bytes,began))return stop(kIOReturnNotReady);
        /* A tick issues at most one SMC. Both the pure state machine and this
         * loop have bounds, including a clock that stops advancing. */
        for(unsigned tick=0;tick<12000;++tick) {
            const uint64_t now=ringNow(nullptr)/1000;
            report_.scm_state=k70_scm_tick(&scm_,now,k70_scm_arm64_invoke,nullptr);
            report_.scm_x0=(int64_t)scm_.result.x[0];report_.scm_x1=scm_.result.x[1];
            if(report_.scm_state!=K70_SCM_PENDING) {
                K70BootLog("K70 GPU: SCM command %u state %u x0=0x%llx x1=0x%llx x6=0x%llx calls=%u resumes=%u busy=%u elapsed=%llums\n",
                    cmd,report_.scm_state,(unsigned long long)scm_.result.x[0],
                    (unsigned long long)scm_.result.x[1],(unsigned long long)scm_.result.x[6],
                    scm_.invocations,scm_.resumes,scm_.busy_count,(unsigned long long)(now-began));
                return report_.scm_state==K70_SCM_COMPLETE?kIOReturnSuccess:stop(kIOReturnIOError);
            }
            if(scm_.next_try>now)IOSleep((unsigned)(scm_.next_try-now));
        }
        report_.scm_state=K70_SCM_INDETERMINATE;return stop(kIOReturnTimeout);
    }
    IOReturn authenticateZap() {
        report_.stage=K70GpuZapMemory;
        /* The reservation shares a 16K page with its lower neighbour. Map a
         * bounded native page with ml_io_map, which does not change neighbour
         * pages' global cache attributes. Refuse every allocator-owned page.
         * The current XNU high-RAM window is independently checked at runtime. */
        const uint64_t page=PAGE_SIZE,pa=K70_ZAP_PHYSICAL&~(page-1);
        zap_mapping_bytes_=(vm_size_t)(((K70_ZAP_PHYSICAL-pa+K70_ZAP_BYTES)+page-1)&~(page-1));
        if(!gPhysSize || gPhysSize>UINT64_MAX-gPhysBase ||
           (pa<gPhysBase+gPhysSize && gPhysBase<pa+zap_mapping_bytes_))return stop(kIOReturnBusy);
        for(uint64_t at=pa;at<pa+zap_mapping_bytes_;at+=page)
            if(pmap_valid_page((ppnum_t)(at/page)))return stop(kIOReturnBusy);
        IOReturn status=zap_metadata_.allocateCpuPages(zap_plan_.metadata_bytes,40,true);
        if(status || zap_metadata_.spanCount()!=1)return stop(status?status:kIOReturnUnsupported);
        uint8_t *meta=static_cast<uint8_t *>(zap_metadata_.mapping());
        memcpy(meta,zap_.bytes,zap_plan_.header_bytes);
        memcpy(meta+zap_plan_.header_bytes,zap_.bytes+zap_plan_.hash_offset,zap_plan_.hash_bytes);
        /* Metadata is retained even after success, and all 16K backing bytes
         * are published, including the initialized padding outside SCM's 4K. */
        if((status=zap_metadata_.holdForDevice()) ||
           (status=NativeAdrenoMaintainOwnedPages(meta,zap_metadata_.ownedBytes(),NativeAdrenoCleanInvalidate)))return stop(status);
        NativeAdrenoPublishBarrier();
        zap_mapping_=ml_io_map_unmappable((vm_offset_t)pa,zap_mapping_bytes_,0x7 /* VM_WIMG_IO */);
        if(!zap_mapping_)return stop(kIOReturnNoMemory);
        report_.stage=K70GpuZapInit;
        if((status=scmCall(1,zap_metadata_.spans()[0].physical,zap_plan_.metadata_bytes)))return status;
        if(zap_plan_.relocatable) {
            report_.stage=K70GpuZapSetup;
            if((status=scmCall(2,K70_ZAP_PHYSICAL,zap_plan_.span)))return status;
        }
        report_.stage=K70GpuZapCopy;
        volatile uint8_t *region=reinterpret_cast<volatile uint8_t *>(zap_mapping_+K70_ZAP_PHYSICAL-pa);
        for(unsigned i=0;i<zap_plan_.count;++i) {
            const auto &segment=zap_plan_.segments[i];
            for(uint32_t b=0;b<segment.file_bytes;++b)region[segment.region_offset+b]=zap_.bytes[segment.file_offset+b];
            for(uint32_t b=segment.file_bytes;b<segment.memory_bytes;++b)region[segment.region_offset+b]=0;
        }
        NativeAdrenoPublishBarrier();
        ml_io_unmap(zap_mapping_,zap_mapping_bytes_);zap_mapping_=0;
        report_.stage=K70GpuZapAuth;
        if((status=scmCall(5)))return status;
        report_.zap_authenticated=true;
        report_.stage=K70GpuSecureMode;
        uint32_t words[2]={},fence=0;
        if(AdrenoPM4Type7(0x66,1,&words[0])!=AdrenoPM4Success)return stop(kIOReturnInvalid);
        if((status=ring_.submit(words,2,&fence)))return stop(status);
        return waitFence(fence);
    }
    static bool map(uint64_t pa,size_t bytes,OSSharedPtr<IOMemoryMap> &out) {
        IODeviceMemory *memory=IODeviceMemory::withRange(pa,bytes);
        if(!memory)return false;
        out=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
        memory->release();
        return out && out->getLength()>=bytes && out->getVirtualAddress();
    }
    int readCore(uint32_t word,uint32_t *out) {
        return device_.io.read32(device_.io.cookie,ShengGmuCore,coreAt(word),out);
    }
    int writeCore(uint32_t word,uint32_t value) {
        return device_.io.write32(device_.io.cookie,ShengGmuCore,coreAt(word),value);
    }
    IOReturn stop(IOReturn status) {
        report_.status=status;
        K70BootLog("K70 GPU: execution stage %u status 0x%x GMU %u context %u firmware %u GX %u\n",
            report_.stage,status,report_.gmu_status,report_.context_step,report_.firmware_error,report_.gx_status);
        K70BootLog("K70 GPU: fence submitted %u observed %u scratch 0x%x ZAP %u ready %u\n",
            report_.submitted_fence,report_.observed_fence,report_.scratch_value,
            report_.zap_authenticated?1:0,report_.execution_ready?1:0);
        return status;
    }
    bool freshSmmu(const K70GpuStartupConfig &c) {
        uint32_t id0=0,id1=0;
        if(smmu_io_.read32(smmu_io_.cookie,0x20,&id0) ||
            smmu_io_.read32(smmu_io_.cookie,0x24,&id1))return false;
        const unsigned slots=id0&0xff,banks=id1&0xff;
        if(!slots || slots>128 || c.gmu_bank>=banks || c.gpu_bank>=banks ||
            c.gmu_slot>=slots || c.gpu_slot0>=slots || c.gpu_slot1>=slots)return false;
        /* A fresh handoff has no active stream ownership. Do not replace a
         * route installed by the previous OS or another owner. */
        for(unsigned slot=0;slot<slots;++slot) {
            uint32_t smr=0;
            if(smmu_io_.read32(smmu_io_.cookie,0x800+slot*4,&smr) || (smr&(1u<<31)))return false;
        }
        const size_t page=(id1&(1u<<31))?0x10000:0x1000;
        const size_t base=page<<(((id1>>28)&7)+1);
        const unsigned owned_banks[2]={c.gmu_bank,c.gpu_bank};
        for(unsigned bank:owned_banks) {
            uint32_t sctlr=0;
            if(smmu_io_.read32(smmu_io_.cookie,base+bank*page,&sctlr) || (sctlr&1))return false;
        }
        return true;
    }
public:
    IOReturn prepare(const K70GpuStartupConfig &c,bool execution) {
        /* Empty weak firmware defaults must fail before board power writes. */
        report_.stage=K70GpuFirmware;
        NativeAdrenoFirmwareRules rules{};
        rules.bytes=SHENG_GMU_FIRMWARE_BYTES;
        memcpy(rules.sha256,sheng_gmu_firmware_sha256,32);
        NativeAdrenoFirmwareView validated{};
        IOReturn status=NativeAdrenoFirmware::validate(native_adreno_firmware_image,
            native_adreno_firmware_image_size,&rules,&validated);
        K70BootLog("K70 GPU: firmware GMU status 0x%x bytes %llu\n",status,
            (unsigned long long)native_adreno_firmware_image_size);
        if(status)return stop(status);
        if(execution) {
            status=NativeAdrenoFirmware::validate(native_adreno_sqe_firmware_image,
                native_adreno_sqe_firmware_image_size,&k70_sqe_rules,&sqe_);
            K70BootLog("K70 GPU: firmware SQE status 0x%x bytes %llu\n",status,
                (unsigned long long)native_adreno_sqe_firmware_image_size);
            if(status)return stop(status);
            status=NativeAdrenoFirmware::validate(native_adreno_zap_image,
                native_adreno_zap_image_size,&k70_zap_rules,&zap_);
            K70BootLog("K70 GPU: firmware Xiaomi ZAP status 0x%x bytes %llu\n",status,
                (unsigned long long)native_adreno_zap_image_size);
            if(status)return stop(status);
            if(!k70_zap_plan(zap_.bytes,zap_.length,&zap_plan_))return stop(kIOReturnInvalid);
        }
        NativeAdrenoCpuState cpu{};
        if((status=NativeAdrenoQueryCpuNative(&cpu)))return stop(status);
        if(cpu.kernel_page_bytes!=16384)return stop(kIOReturnUnsupported);
        report_.stage=K70GpuPower;
        for(unsigned i=0;i<3;++i) {
            if(!map(power_phys_[i],power_bytes_[i],power_maps_[i]))return stop(kIOReturnNoMemory);
            power_[i]=reinterpret_cast<volatile uint8_t *>(power_maps_[i]->getVirtualAddress());
        }
        const gpb_io pio{this,powerRead,powerWrite,delay};
        gpb_result power{};
        /* Only the GCC/GPUCC windows are offered here. SMMU and GX registers
         * cannot be read by the power helper before their prerequisites. */
        gpb_run(&pio,3,7,&power);
        if(power.io_error || power.cc!=GPB_DONE || !gpb_gdsc_on(power.cx_gdscr))
            return stop(kIOReturnNotReady);
        /* A bootloader can leave CX on with its interface clocks off. The
         * shared power helper only enables these branches when it raises CX;
         * ensure them for the already-on case as well. */
        if(powerWrite(this,GPB_WIN_GPUCC,GPB_CC_AHB_CLK,power.cc_ahb|GPB_CBCR_ENABLE) ||
            powerWrite(this,GPB_WIN_GPUCC,GPB_CC_SMMU_VOTE_CLK,power.cc_smmu|GPB_CBCR_ENABLE))
            return stop(kIOReturnIOError);
        for(unsigned i=0;i<50;++i) {
            if(powerRead(this,GPB_WIN_GPUCC,GPB_CC_SMMU_VOTE_CLK,&power.cc_smmu))
                return stop(kIOReturnIOError);
            if(gpb_clock_on(power.cc_smmu))break;IODelay(10);
        }
        if(powerRead(this,GPB_WIN_GPUCC,GPB_CC_AHB_CLK,&power.cc_ahb) ||
            !gpb_clock_on(power.cc_smmu) || !gpb_clock_on(power.cc_ahb))return stop(kIOReturnNotReady);
        report_.stage=K70GpuClocks;
        gpb_gmu_clocks clocks{};
        gpb_read_gmu_clocks(&pio,&power,&clocks);
        gpb_gmu_clock_on enabled{};
        gpb_gmu_clocks_on(&pio,&power,&clocks,&enabled);
        report_.platform_status=enabled.failed;
        if(power.io_error || !clocks.done || enabled.step!=7 || enabled.failed)
            return stop(kIOReturnNotReady);
        gpb_read_gmu_clocks(&pio,&power,&clocks);
        if(power.io_error || !gpb_gmu_regs_readable(&power,&clocks))return stop(kIOReturnNotReady);
        NativeAdrenoGmuWindow windows[ShengGmuWindows]{};
        for(unsigned i=0;i<ShengGmuWindows;++i) {
            if(!map(sheng_gmu_window_physical[i],sheng_gmu_window_bytes[i],gmu_maps_[i]))
                return stop(kIOReturnNoMemory);
            windows[i]={reinterpret_cast<volatile uint8_t *>(gmu_maps_[i]->getVirtualAddress()),
                sheng_gmu_window_bytes[i]};
        }
        status=platform_.bind(windows,&device_.io);
        if(status)return stop(status);
        time_={nullptr,NativeAdrenoKernelTime::now_us,NativeAdrenoKernelTime::wait_us};
        if((status=platform_.services(&time_,&device_.services)))return stop(status);
        report_.stage=K70GpuIdle;
        uint32_t reset=0,hfi=0,retention=0,busy=0;
        if(readCore(REG_GMU_CM3_SYSRESET,&reset)) {
            K70BootLog("K70 GPU: idle read failed GMU SYSRESET\n"); return stop(kIOReturnIOError);
        }
        if(readCore(REG_GMU_HFI_CTRL_STATUS,&hfi)) {
            K70BootLog("K70 GPU: idle read failed GMU HFI_CTRL_STATUS\n"); return stop(kIOReturnIOError);
        }
        if(device_.io.read32(device_.io.cookie,ShengGmuCxMisc,REG_CX_MISC_TCM_RET_CNTL*4,&retention)) {
            K70BootLog("K70 GPU: idle read failed CX_MISC TCM_RET_CNTL\n"); return stop(kIOReturnIOError);
        }
        if(device_.io.read32(device_.io.cookie,ShengGmuRscc,REG_RSCC_SEQ_BUSY_DRV0*4,&busy)) {
            K70BootLog("K70 GPU: idle read failed RSCC SEQ_BUSY\n"); return stop(kIOReturnIOError);
        }
        K70BootLog("K70 GPU: idle reset=0x%x hfi=0x%x retention=0x%x rscc_busy=0x%x\n",
            reset,hfi,retention,busy);
        if(reset!=1 || hfi || retention || busy)return stop(kIOReturnBusy);
        if(!map(SHENG_GPU_SMMU_BASE,SHENG_GPU_SMMU_BYTES,smmu_map_))return stop(kIOReturnNoMemory);
        smmu_mmio_={reinterpret_cast<volatile unsigned char *>(smmu_map_->getVirtualAddress()),SHENG_GPU_SMMU_BYTES};
        if(ArmSmmu4KBindMmio(&smmu_mmio_,&smmu_io_)!=ArmSmmu4KSuccess ||
            !freshSmmu(c))return stop(kIOReturnBusy);
        report_.stage=K70GpuCommandDb;
        if(!map(SHENG_GMU_COMMAND_DB_BASE,SHENG_GMU_COMMAND_DB_BYTES,db_map_))return stop(kIOReturnNoMemory);
        const auto *db=reinterpret_cast<const volatile uint32_t *>(db_map_->getVirtualAddress());
        /* Device memory requires aligned 32-bit reads. Parse owned normal RAM,
         * keeping every auxiliary table pointer alive for the entire startup. */
        for(size_t i=0;i<sizeof command_db_/4;++i) {
            const uint32_t word=db[i];memcpy(command_db_+i*4,&word,4);
        }
        uint32_t version=0;memcpy(&version,command_db_,4);
        ShengGmuVoteInputs votes{};
        report_.command_db_status=ShengGmuCommandDbInputs(command_db_,sizeof command_db_,version,&votes);
        if(report_.command_db_status!=ShengGmuDbSuccess)return stop(kIOReturnInvalid);
        votes.chip_id=SHENG_GMU_VOTES_CHIP_ID;
        votes.enable_acd=1;votes.enable_ifpc=0;
        report_.votes_status=ShengGmuBuildStartup(&votes,&startup_);
        if(report_.votes_status!=AdrenoHfiStartupSuccess)return stop(kIOReturnInvalid);
        report_.stage=K70GpuGmuContext;
        NativeAdrenoGmuContextReport context{};
        status=gmu_context_.bringUp(&smmu_io_,c.gmu_bank,c.gmu_asid,c.gmu_slot,
            SHENG_GPU_SMMU_GMU_SID,nullptr,&context);
        report_.context_step=context.step;
        if(status)return stop(status);
        NativeAdrenoGmuAllocator &allocation=gmu_context_.allocator();
        NativeAdrenoMemory *objects[5]={&allocation.icache(),&allocation.dummy(),&allocation.debug(),
            &allocation.log(),&allocation.hfi()};
        const auto &layout=allocation.layout();
        const uint64_t iovas[5]={layout.icache_iova,layout.dummy_iova,layout.debug_iova,layout.log_iova,layout.hfi_iova};
        ShengGmuMemory *payloads[5]={&firmware_config_.icache,&firmware_config_.dummy,
            &firmware_config_.debug,&firmware_config_.log,&firmware_config_.hfi};
        for(unsigned i=0;i<5;++i)
            *payloads[i]={static_cast<volatile uint32_t *>(objects[i]->mapping()),objects[i]->bytes(),iovas[i]};
        firmware_config_.chip_id=SHENG_GMU_CHIP_ID;
        firmware_config_.firmware=validated.bytes;
        firmware_config_.firmware_bytes=validated.length;
        firmware_config_.poll_budget=200;
        report_.stage=K70GpuHfi;
        /* a6xx_gmu_resume clears AO faults before releasing the CM3. */
        constexpr uint32_t aoClear=0x23b04,aoMask=0x23b06;
        if(writeCore(aoClear,~0u) || writeCore(aoMask,~SHENG_GMU_AO_FATAL_MASK) ||
            device_.io.barrier(device_.io.cookie))return stop(kIOReturnIOError);
        const ShengGmuStartupLimits limits{1000,100,50000,2000000};
        report_.gmu_status=ShengGmuColdStart(&device_,&firmware_config_,&startup_,&limits,&startup_result_);
        report_.firmware_error=startup_result_.protocol.firmware_error;
        if(report_.gmu_status!=ShengGmuSuccess)return stop(kIOReturnIOError);
        report_.gmu_ready=true;
        report_.stage=K70GpuFrequency;
        constexpr uint32_t g2hClear=0x1f991,g2hMask=0x1f993,g2hInfo=0x1f992,h2gSet=0x1f994,gxStatus=0x1f8d0;
        if(writeCore(g2hClear,~0u) || writeCore(g2hMask,~SHENG_GMU_CM3_FATAL_MASK) ||
            device_.io.barrier(device_.io.cookie))return stop(kIOReturnIOError);
        unsigned bandwidth=0;
        const unsigned performance=ShengGmuPerfVote(c.performance_level,&bandwidth);
        report_.performance_level=performance;report_.bandwidth_level=bandwidth;
        const uint32_t vote[3]={1,performance,bandwidth};
        uint32_t reply[16]{},error=0;size_t count=0;
        const AdrenoHfiStatus exchanged=AdrenoHfiExchange(&device_.hfi,30,vote,3,1000,
            hfiWait,reply,16,&count,&error);
        report_.firmware_error=error;
        if(exchanged!=AdrenoHfiSuccess || error)return stop(kIOReturnIOError);
        report_.stage=K70GpuGx;
        /* OOB GPU_SET pins GX on during kernel initialization. Only poll the
         * always-on GMU block until the GMU confirms GX power and clocks. */
        if(writeCore(h2gSet,1u<<30) || device_.io.barrier(device_.io.cookie))return stop(kIOReturnIOError);
        bool acknowledged=false;
        for(unsigned i=0;i<100;++i) {
            uint32_t info=0;if(readCore(g2hInfo,&info))return stop(kIOReturnIOError);
            if(info&(1u<<31)) {acknowledged=true;break;}IODelay(100);
        }
        if(!acknowledged)return stop(kIOReturnTimeout);
        gx_held_=true;
        if(writeCore(g2hClear,1u<<31) || device_.io.barrier(device_.io.cookie) ||
            readCore(gxStatus,&report_.gx_status))return stop(kIOReturnIOError);
        if(report_.gx_status&3)return stop(kIOReturnNotReady);
        if((status=setGpuActlr()))return status;
        report_.stage=K70GpuAddressSpace;
        const unsigned slots[2]={c.gpu_slot0,c.gpu_slot1};
        status=gpu_context_.bringUp(&smmu_io_,c.gpu_bank,c.gpu_asid,slots,
            sheng_gpu_smmu_sids,nullptr,&context);
        report_.context_step=context.step;
        if(status)return stop(status);
        report_.gpu_address_space_ready=true;
        report_.stage=K70GpuGmuPrepared;
        if(execution)return startCp();
        return stop(kIOReturnSuccess);
    }
    IOReturn startCp() {
        IOReturn status;
        report_.stage=K70GpuSqe;
        auto &sqe=gpu_context_.buffer(0);
        if(sqe_.length<8 || sqe_.length-4>sqe.bytes())return stop(kIOReturnInvalid);
        memcpy(sqe.mapping(),sqe_.bytes+4,sqe_.length-4);
        if((status=sqe.publishForDevice(0,sqe.bytes(),false)))return stop(status);
        NativeAdrenoPublishBarrier();
        if(memcmp(sqe.mapping(),sqe_.bytes+4,sqe_.length-4))return stop(kIOReturnIOError);
        report_.stage=K70GpuHwInit;
        ring_io_={this,gpuWrite,gpuRead,gpuBarrier};
        const K70GpuHwIo hw{ring_io_,gmuWrite};
        if((status=k70_gpu_hw_init(&hw,GpuSqeIova)))return stop(status);
        report_.stage=K70GpuRing;
        auto &rb=gpu_context_.buffer(1);auto &fence_memory=gpu_context_.buffer(2);
        const NativeAdrenoRingCache cache{this,ringPublish,ringAcquire};
        ring_clock_={nullptr,ringNow,ringIdle};
        /* Context mappings already hold device references for these permanent
         * buffers. Use their owned CPU views; never remap their physical pages. */
        if((status=ring_.attach(&ring_io_,&cache,rb.mapping(),rb.bytes(),GpuRingIova,
            fence_memory.mapping(),fence_memory.bytes(),GpuMemptrsIova)) ||
           (status=fence_memory.publishForDevice(0,fence_memory.bytes(),true)) ||
           (status=gpuWrite64(A740_CP_BV_RB_RPTR_ADDR,GpuMemptrsIova+8)))return stop(status);
        if(gpuWrite(this,A740_CP_SQE_CNTL,1) || gpuBarrier(this))return stop(kIOReturnIOError);
        report_.stage=K70GpuCpInit;
        auto &pwrup=gpu_context_.buffer(3);size_t written=0;
        if((status=NativeAdrenoPwrupList::buildA740(gpuRead,this,static_cast<uint8_t *>(pwrup.mapping()),pwrup.bytes(),&written)) ||
           (status=pwrup.publishForDevice(0,pwrup.bytes(),false)))return stop(status);
        const NativeAdrenoCpInitConfig init{GpuSqeIova,GpuPwrupIova,1};
        uint32_t fence=0;
        if((status=cp_.run(&ring_io_,&ring_,&init,&fence)))return stop(status);
        if((status=waitFence(fence)))return status;
        report_.stage=K70GpuFirstWrite;
        auto &scratch=gpu_context_.buffer(4);
        auto *word=static_cast<volatile uint32_t *>(scratch.mapping());*word=0;
        if((status=scratch.publishForDevice(0,scratch.bytes(),true)))return stop(status);
        uint32_t words[4]={};
        if(AdrenoPM4Type7(0x3d,3,&words[0])!=AdrenoPM4Success)return stop(kIOReturnInvalid);
        words[1]=uint32_t(GpuScratchIova);words[2]=uint32_t(GpuScratchIova>>32);words[3]=0x5e9a740;
        if((status=ring_.submit(words,4,&fence)))return stop(status);
        if((status=waitFence(fence)))return status;
        if((status=scratch.acquireDeviceWrites(0,scratch.bytes())))return stop(status);
        report_.scratch_value=*word;
        if(report_.scratch_value!=words[3])return stop(kIOReturnIOError);
        if((status=authenticateZap()))return status;
        if((status=submit_.attach(gpu_context_.vm(),&ring_)))return stop(status);
        const NativeAdrenoTimeIo timer_io{this,gpuRead};
        if((status=timer_.attach(&timer_io,NativeAdrenoTimeCp)))return stop(status);
        uint64_t ticks=0;if((status=timer_.read(&ticks)))return stop(status);
        report_.stage=K70GpuExecutionReady;report_.execution_ready=true;
        return stop(kIOReturnSuccess);
    }
    IOReturn execution(K70GpuExecution *out) {
        if(!report_.execution_ready)return kIOReturnNotReady;
        *out={gpu_context_.vm(),&submit_,&ring_,&ring_clock_,&timer_};return kIOReturnSuccess;
    }
    const K70GpuStartupReport &report() const {return report_;}
};
K70GpuStartup *owner;
uint32_t attempted;
}

static IOReturn start(const K70GpuStartupConfig *config,K70GpuStartupReport *report,bool execution)
{
    if(!report)return kIOReturnBadArgument;
    *report={};report->stage=K70GpuValidate;
    /* qcom GPU contexts are bank0/ASID2/slots0,1; GMU is bank1/ASID1/slot2.
     * Board callers cannot substitute a different layout on K70. */
    if(!config || config->gmu_bank!=1 || config->gpu_bank!=0 ||
        config->gmu_asid!=1 || config->gpu_asid!=2 || config->gmu_slot!=2 ||
        config->gpu_slot0!=0 || config->gpu_slot1!=1 || !config->performance_level ||
        config->performance_level>=SHENG_GPU_OPPS) {
        report->status=kIOReturnBadArgument;return report->status;
    }
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        report->status=kIOReturnBusy;return report->status;
    }
    /* A permanent owner preserves live tables and MMIO even on an uncertain
     * device failure. Never destroy objects while DMA ownership is unresolved. */
    owner=new K70GpuStartup;
    if(!owner) {report->status=kIOReturnNoMemory;return report->status;}
    const IOReturn status=owner->prepare(*config,execution);
    *report=owner->report();__atomic_store_n(&attempted,2,__ATOMIC_RELEASE);return status;
}
extern "C" IOReturn k70_gpu_prepare_gmu(const K70GpuStartupConfig *config,K70GpuStartupReport *report)
{return start(config,report,false);}
extern "C" IOReturn k70_gpu_start_execution(const K70GpuStartupConfig *config,K70GpuStartupReport *report)
{return start(config,report,true);}
extern "C" IOReturn k70_gpu_execution(K70GpuExecution *out)
{
    if(!out)return kIOReturnBadArgument;
    *out={};return __atomic_load_n(&attempted,__ATOMIC_ACQUIRE)==2 && owner?owner->execution(out):kIOReturnNotReady;
}
