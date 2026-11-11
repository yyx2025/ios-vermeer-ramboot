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

extern "C" const uint8_t native_adreno_firmware_image[];
extern "C" const uint64_t native_adreno_firmware_image_size;

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
    IOReturn stop(IOReturn status) {report_.status=status;return status;}
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
    IOReturn prepare(const K70GpuStartupConfig &c) {
        /* Empty weak firmware defaults must fail before board power writes. */
        report_.stage=K70GpuFirmware;
        NativeAdrenoFirmwareRules rules{};
        rules.bytes=SHENG_GMU_FIRMWARE_BYTES;
        memcpy(rules.sha256,sheng_gmu_firmware_sha256,32);
        NativeAdrenoFirmwareView validated{};
        IOReturn status=NativeAdrenoFirmware::validate(native_adreno_firmware_image,
            native_adreno_firmware_image_size,&rules,&validated);
        if(status)return stop(status);
        NativeAdrenoCpuState cpu{};
        if((status=NativeAdrenoQueryCpuNative(&cpu)))return stop(status);
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
        if(readCore(REG_GMU_CM3_SYSRESET,&reset) || readCore(REG_GMU_HFI_CTRL_STATUS,&hfi) ||
            device_.io.read32(device_.io.cookie,ShengGmuCxMisc,REG_CX_MISC_TCM_RET_CNTL*4,&retention) ||
            device_.io.read32(device_.io.cookie,ShengGmuRscc,REG_RSCC_SEQ_BUSY_DRV0*4,&busy))
            return stop(kIOReturnIOError);
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
        report_.stage=K70GpuAddressSpace;
        const unsigned slots[2]={c.gpu_slot0,c.gpu_slot1};
        status=gpu_context_.bringUp(&smmu_io_,c.gpu_bank,c.gpu_asid,slots,
            sheng_gpu_smmu_sids,nullptr,&context);
        report_.context_step=context.step;
        if(status)return stop(status);
        report_.gpu_address_space_ready=true;
        report_.stage=K70GpuGmuPrepared;
        /* CP/SQE, authenticated K70 ZAP, first completed fence and scanout
         * remain required. Callers must not expose this as execution_ready. */
        return stop(kIOReturnSuccess);
    }
    const K70GpuStartupReport &report() const {return report_;}
};
K70GpuStartup *owner;
uint32_t attempted;
}

extern "C" IOReturn k70_gpu_prepare_gmu(const K70GpuStartupConfig *config,K70GpuStartupReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};report->stage=K70GpuValidate;
    if(!config || config->gmu_bank>63 || config->gpu_bank>63 ||
        config->gmu_bank==config->gpu_bank || !config->gmu_asid || !config->gpu_asid ||
        config->gmu_asid>0xffff || config->gpu_asid>0xffff || config->gmu_asid==config->gpu_asid ||
        config->gmu_slot>127 || config->gpu_slot0>127 || config->gpu_slot1>127 ||
        config->gmu_slot==config->gpu_slot0 || config->gmu_slot==config->gpu_slot1 ||
        config->gpu_slot0==config->gpu_slot1 || !config->performance_level ||
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
    const IOReturn status=owner->prepare(*config);
    *report=owner->report();return status;
}
