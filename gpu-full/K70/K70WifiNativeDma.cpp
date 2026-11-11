/* SPDX-License-Identifier: MIT */
#include "K70WifiNativeDma.h"
#include <string.h>

namespace {
constexpr size_t TablePages=128;
constexpr size_t TableBytes=TablePages*4096;
constexpr unsigned PollBudget=1000000;

static IOReturn smmuReturn(ArmSmmu4KStatus status)
{
    switch(status) {
    case ArmSmmu4KSuccess:return kIOReturnSuccess;
    case ArmSmmu4KArgument:
    case ArmSmmu4KBadRange:
    case ArmSmmu4KOverlap:return kIOReturnBadArgument;
    case ArmSmmu4KCapacity:
    case ArmSmmu4KBudget:return kIOReturnNoResources;
    case ArmSmmu4KUnsupported:return kIOReturnUnsupported;
    case ArmSmmu4KBusy:return kIOReturnBusy;
    case ArmSmmu4KTimeout:return kIOReturnTimeout;
    case ArmSmmu4KStateMismatch:
    case ArmSmmu4KIoError:
    case ArmSmmu4KFault:return kIOReturnIOError;
    }
    return kIOReturnIOError;
}

static unsigned widthFromCode(unsigned code)
{
    switch(code) {case 0:return 32;case 1:return 36;case 2:return 40;
        case 3:return 42;case 4:return 44;case 5:return 48;default:return 0;}
}

static unsigned chooseWidth(uint64_t last,unsigned supported)
{
    const unsigned widths[]={32,36,40,42,44,48};
    for(unsigned width:widths) {
        const uint64_t limit=UINT64_C(1)<<width;
        if(width<=supported && last<limit)return width;
    }
    return 0;
}

static bool rangeEnd(uint64_t base,uint64_t bytes,uint64_t *last)
{
    if(!bytes || base>UINT64_MAX-(bytes-1))return false;
    *last=base+bytes-1;return true;
}

static bool platformValid(const K70WifiNativeDmaPlatform *p)
{
    return p && p->acquire && p->lock && p->unlock && p->prepare_platform &&
        p->quiesce && p->verify_quiesced && p->release_lease;
}
}

IOReturn K70WifiNativeDma::enter()
{
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&gate_,&expected,1u,false,__ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE)?kIOReturnSuccess:kIOReturnBusy;
}

void K70WifiNativeDma::leave()
{
    __atomic_store_n(&gate_,0u,__ATOMIC_RELEASE);
}

IOReturn K70WifiNativeDma::fail(IOReturn status,ArmSmmu4KStatus smmu)
{
    report_.status=status;report_.smmu_status=smmu;
    report_.state=K70WifiNativeDmaQuarantined;
    if(arena_)k70_wifi_dma_quarantine(arena_);
    return status;
}

int K70WifiNativeDma::syncOwned(void *cookie,void *cpu,size_t bytes,int direction)
{
    auto *self=static_cast<K70WifiNativeDma *>(cookie);
    if(!self || !cpu || !bytes || direction<0 || direction>2 ||
       self->report_.state!=K70WifiNativeDmaActive || !self->data_.mapping())return -1;
    const uintptr_t base=(uintptr_t)self->data_.mapping(),at=(uintptr_t)cpu;
    if(at<base || at-base>self->data_.bytes() || bytes>self->data_.bytes()-(at-base))return -1;
    /* Linux's single callback is used both before and after DMA. Clean for
     * TO_DEVICE; clean+invalidate for FROM/BIDIRECTIONAL is conservative on
     * both edges and never substitutes a device-completion fence. */
    const IOReturn status=self->data_.publishForDevice((size_t)(at-base),bytes,direction!=1);
    if(status!=kIOReturnSuccess) {
        self->data_.quarantine();
        if(self->arena_)k70_wifi_dma_quarantine(self->arena_);
        self->report_.status=status;self->report_.state=K70WifiNativeDmaQuarantined;
        return -1;
    }
    return 0;
}

IOReturn K70WifiNativeDma::observeLocked(bool audit_tables)
{
    if(!programmed_ || !attached_)return kIOReturnNotReady;
    ArmSmmu4KStatus status;
    if(audit_tables) {
        ArmSmmu4KAuditResult audit{};
        status=ArmSmmu4KObserveAuditedContext(&io_,lease_.bank,&context_,&audit_config_,&audit);
        if(status!=ArmSmmu4KSuccess)return fail(smmuReturn(status),status);
    }
    ArmSmmu4KStreamResult stream{};
    status=ArmSmmu4KObserveStreams(&io_,&streams_,&context_,&stream);
    if(status!=ArmSmmu4KSuccess)return fail(smmuReturn(status),status);
    report_.id0=stream.id0;report_.id1=stream.context.id1;report_.id2=stream.context.id2;
    return kIOReturnSuccess;
}

IOReturn K70WifiNativeDma::stopLocked()
{
    if(report_.state==K70WifiNativeDmaStopped)return kIOReturnSuccess;
    if(!lease_.owner_cookie)return kIOReturnNotReady;
    IOReturn rc=platform_.quiesce(platform_.cookie,&lease_);
    if(rc!=kIOReturnSuccess)return fail(rc);
    rc=platform_.verify_quiesced(platform_.cookie,&lease_);
    if(rc!=kIOReturnSuccess)return fail(rc);
    if(attached_) {
        ArmSmmu4KStreamResult result{};
        const ArmSmmu4KStatus status=ArmSmmu4KFaultStreams(&io_,&streams_,&result);
        report_.streams_faulted=status==ArmSmmu4KSuccess && result.streams_faulted==streams_.count;
        if(status!=ArmSmmu4KSuccess)return fail(smmuReturn(status),status);
        attached_=false;
    }
    if(programmed_) {
        ArmSmmu4KProgramResult sync{};
        ArmSmmu4KStatus status=ArmSmmu4KInvalidateAllOwnedContext(&io_,lease_.bank,
            &context_,PollBudget,&sync);
        if(status!=ArmSmmu4KSuccess)return fail(smmuReturn(status),status);
        ArmSmmu4KProgramResult disabled{};
        status=ArmSmmu4KDisableOwnedContext(&io_,lease_.bank,&context_,PollBudget,&disabled);
        report_.context_disabled=status==ArmSmmu4KSuccess && disabled.disabled_verified &&
            disabled.tlb_synchronized && !disabled.must_retain_tables;
        if(status!=ArmSmmu4KSuccess || !report_.context_disabled)
            return fail(status==ArmSmmu4KSuccess?kIOReturnIOError:smmuReturn(status),status);
        programmed_=false;
    }
    report_.state=K70WifiNativeDmaStopped;report_.status=kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn K70WifiNativeDma::claim(const K70WifiNativeDmaPlatform *platform,uint32_t sid,
    size_t bytes,K70WifiDmaArena *arena)
{
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(report_.state!=K70WifiNativeDmaCold || !platformValid(platform) || sid!=0x1401 ||
       !bytes || bytes>MaxArenaBytes || (bytes&16383) || !arena) {leave();return kIOReturnBadArgument;}
    platform_=*platform;arena_=arena;report_.state=K70WifiNativeDmaClaiming;report_.stage=1;
    rc=platform_.acquire(platform_.cookie,sid,bytes,&lease_);
    report_.lease_retained=lease_.owner_cookie!=nullptr;
    if(rc!=kIOReturnSuccess || !lease_.owner_cookie) {rc=fail(rc?rc:kIOReturnUnsupported);leave();return rc;}
    if(lease_.sid!=sid || !lease_.smmu_mapping || lease_.smmu_physical!=AppsSmmuPhysical ||
       lease_.smmu_bytes<0x10000 || lease_.dma_bytes!=bytes || !lease_.dma_iova || !lease_.msi_iova ||
       (lease_.dma_iova&16383) || (lease_.msi_iova&4095) || lease_.bank>255 ||
       lease_.asid>65535 || lease_.stream_slot>255 || lease_.coherent_walk>1) {
        rc=fail(kIOReturnUnsupported);leave();return rc;
    }
    uint64_t dma_last=0,msi_last=0;
    if(!rangeEnd(lease_.dma_iova,bytes,&dma_last) ||
       !rangeEnd(lease_.msi_iova,4096,&msi_last) ||
       (lease_.dma_iova<=msi_last && lease_.msi_iova<=dma_last)) {
        rc=fail(kIOReturnBadArgument);leave();return rc;
    }
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc!=kIOReturnSuccess) {rc=fail(rc);leave();return rc;}
    platform_locked_=true;report_.stage=2;
    rc=platform_.prepare_platform(platform_.cookie,&lease_);
    if(rc!=kIOReturnSuccess)goto out_failed;
    mmio_.mapping=lease_.smmu_mapping;mmio_.bytes=lease_.smmu_bytes;
    {
        const ArmSmmu4KStatus status=ArmSmmu4KBindMmio(&mmio_,&io_);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
    }
    if(io_.read32(io_.cookie,0x20,&report_.id0) || io_.read32(io_.cookie,0x24,&report_.id1) ||
       io_.read32(io_.cookie,0x28,&report_.id2)) {rc=kIOReturnIOError;goto out_failed;}
    report_.stage=3;
    rc=data_.allocateCpuPages(bytes,48,true);if(rc!=kIOReturnSuccess)goto out_failed;
    if(data_.spanCount()!=1 || data_.bytes()!=bytes || data_.ownedBytes()!=bytes ||
       !data_.mapping() || !data_.cpuPageBytes()) {rc=kIOReturnUnsupported;goto out_failed;}
    rc=data_.holdForDevice();if(rc!=kIOReturnSuccess)goto out_failed;data_held_=true;
    rc=tables_.allocateCpuPages(TableBytes,48,true);if(rc!=kIOReturnSuccess)goto out_failed;
    if(tables_.spanCount()!=1 || tables_.bytes()!=TableBytes || tables_.ownedBytes()!=TableBytes ||
       !tables_.mapping()) {rc=kIOReturnUnsupported;goto out_failed;}
    rc=tables_.holdForDevice();if(rc!=kIOReturnSuccess)goto out_failed;tables_held_=true;
    report_.arena_bytes=bytes;report_.cpu_page_bytes=data_.cpuPageBytes();
    report_.dma_iova=lease_.dma_iova;report_.dma_physical=data_.spans()[0].physical;
    report_.table_physical=tables_.spans()[0].physical;report_.msi_iova=lease_.msi_iova;
    {
        uint64_t physical_last=ItsDoorbellPhysical+4095;
        const uint64_t data_end=data_.spans()[0].physical+bytes-1;
        const uint64_t table_end=tables_.spans()[0].physical+TableBytes-1;
        if(data_end>physical_last)physical_last=data_end;if(table_end>physical_last)physical_last=table_end;
        const unsigned output_supported=widthFromCode((report_.id2>>4)&15);
        const unsigned input_supported=widthFromCode((report_.id2>>8)&15);
        const uint64_t iova_last=dma_last>msi_last?dma_last:msi_last;
        const unsigned output=chooseWidth(physical_last,output_supported);
        const unsigned input=chooseWidth(iova_last,input_supported);
        if(!output || !input) {rc=kIOReturnUnsupported;goto out_failed;}
        table_config_.input_bits=input;table_config_.output_bits=output;
        table_config_.table_physical=tables_.spans()[0].physical;
        table_config_.leaf_budget=bytes/4096+1;
    }
    ranges_[0]={lease_.dma_iova,data_.spans()[0].physical,bytes,
        ArmSmmu4KRead|ArmSmmu4KWrite,ArmSmmu4KNormalWB,3};
    ranges_[1]={lease_.msi_iova,ItsDoorbellPhysical,4096,
        ArmSmmu4KRead|ArmSmmu4KWrite,ArmSmmu4KDeviceNGnRE,2};
    if(ranges_[1].iova<ranges_[0].iova) {const ArmSmmu4KRange swap=ranges_[0];ranges_[0]=ranges_[1];ranges_[1]=swap;}
    report_.stage=4;
    {
        ArmSmmu4KStatus status=ArmSmmu4KBuild(&table_config_,ranges_,2,
            static_cast<uint64_t *>(tables_.mapping()),TablePages,&table_result_);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
        report_.table_pages=table_result_.table_pages;
        rc=tables_.publishForDevice(0,table_result_.table_pages*4096,false);
        if(rc!=kIOReturnSuccess)goto out_failed;
        rc=data_.publishForDevice(0,bytes,true);if(rc!=kIOReturnSuccess)goto out_failed;
        NativeAdrenoPublishBarrier();
    }
    table_memory_.count=1;
    table_memory_.pools[0].mapping=static_cast<volatile uint64_t *>(tables_.mapping());
    table_memory_.pools[0].physical=tables_.spans()[0].physical;
    table_memory_.pools[0].pages=TablePages;
    {
        ArmSmmu4KStatus status=ArmSmmu4KBindTableMemory(&table_memory_,&table_io_);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
    }
    memset(visited_,0,sizeof(visited_));
    audit_region_.ranges=ranges_;audit_region_.range_count=2;audit_region_.capacity_pages=TablePages;
    audit_region_.read_budget=TablePages*512;audit_region_.visited=visited_;audit_region_.visited_bytes=sizeof(visited_);
    audit_config_.io=table_io_;audit_config_.lower=&audit_region_;audit_config_.upper=nullptr;
    context_.lower=&table_config_;context_.lower_tables=&table_result_;context_.upper=nullptr;
    context_.upper_tables=nullptr;context_.id0=report_.id0;context_.id2=report_.id2;
    context_.asid=lease_.asid;context_.coherent_walk=lease_.coherent_walk;
    context_.stall_on_fault=1;context_.continue_after_fault=0;
    {
        ArmSmmu4KAuditResult audit{};
        const ArmSmmu4KStatus status=ArmSmmu4KAuditDeclaredContext(&context_,&audit_config_,&audit);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
    }
    route_.slot=lease_.stream_slot;route_.sid=(uint16_t)sid;
    {
        ArmSmmu4KStreamResult preflight{};
        ArmSmmu4KStatus status=ArmSmmu4KCheckFaultedStreams(&io_,&route_,1,lease_.bank,&preflight);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
        report_.stage=5;report_.context_attempted=true;
        ArmSmmu4KProgramResult programmed{};
        status=ArmSmmu4KProgramDisabledContext(&io_,lease_.bank,&context_,PollBudget,&programmed);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
        programmed_=true;
        report_.stage=6;attach_attempted_=true;
        ArmSmmu4KStreamResult attached{};
        status=ArmSmmu4KAttachStreams(&io_,&streams_,&route_,1,lease_.bank,&context_,&attached);
        if(status!=ArmSmmu4KSuccess) {rc=smmuReturn(status);report_.smmu_status=status;goto out_failed;}
        attached_=true;
    }
    report_.stage=7;rc=observeLocked(true);if(rc!=kIOReturnSuccess)goto out_failed;
    report_.stage=8;
    if(k70_wifi_dma_init(arena_,data_.mapping(),lease_.dma_iova,bytes,data_.cpuPageBytes(),
        this,&K70WifiNativeDma::syncOwned)) {rc=kIOReturnUnsupported;goto out_failed;}
    report_.software_mapping_ready=true;report_.state=K70WifiNativeDmaActive;
    report_.status=kIOReturnSuccess;report_.smmu_status=ArmSmmu4KSuccess;
    platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;leave();return kIOReturnSuccess;

out_failed:
    if(platform_locked_ && (programmed_ || attached_)) (void)stopLocked();
    fail(rc,report_.smmu_status);
    if(platform_locked_) {platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;}
    leave();return rc;
}

IOReturn K70WifiNativeDma::observe()
{
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(report_.state!=K70WifiNativeDmaActive) {leave();return kIOReturnNotReady;}
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc==kIOReturnSuccess) {platform_locked_=true;rc=observeLocked(true);
        platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;}
    leave();return rc;
}

IOReturn K70WifiNativeDma::mapMsiDoorbell(uint64_t physical,size_t bytes,uint64_t *iova)
{
    if(iova)*iova=0;
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(!iova || physical!=ItsDoorbellPhysical || bytes!=4096 ||
       report_.state!=K70WifiNativeDmaActive || report_.doorbell_references==UINT32_MAX) {
        leave();return kIOReturnBadArgument;
    }
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc==kIOReturnSuccess) {
        platform_locked_=true;rc=observeLocked(false);
        if(rc==kIOReturnSuccess) {++report_.doorbell_references;*iova=lease_.msi_iova;}
        platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;
    }
    leave();return rc;
}

IOReturn K70WifiNativeDma::unmapMsiDoorbell(uint64_t physical,size_t bytes,uint64_t iova)
{
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(physical!=ItsDoorbellPhysical || bytes!=4096 || iova!=lease_.msi_iova ||
       report_.state!=K70WifiNativeDmaStopped || !report_.doorbell_references) {
        leave();return kIOReturnBadArgument;
    }
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc==kIOReturnSuccess) {
        platform_locked_=true;rc=platform_.verify_quiesced(platform_.cookie,&lease_);
        if(rc==kIOReturnSuccess)--report_.doorbell_references;
        platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;
    }
    leave();return rc;
}

IOReturn K70WifiNativeDma::stop()
{
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(report_.state!=K70WifiNativeDmaActive && report_.state!=K70WifiNativeDmaQuarantined &&
       report_.state!=K70WifiNativeDmaStopped) {leave();return kIOReturnNotReady;}
    if(report_.state==K70WifiNativeDmaStopped) {leave();return kIOReturnSuccess;}
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc==kIOReturnSuccess) {platform_locked_=true;rc=stopLocked();
        platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;}
    leave();return rc;
}

IOReturn K70WifiNativeDma::release()
{
    IOReturn rc=enter();if(rc!=kIOReturnSuccess)return rc;
    if(report_.state==K70WifiNativeDmaReleased) {leave();return kIOReturnSuccess;}
    if(!lease_.owner_cookie) {leave();return kIOReturnNotReady;}
    rc=platform_.lock(platform_.cookie,&lease_);
    if(rc!=kIOReturnSuccess) {leave();return fail(rc);}
    platform_locked_=true;
    if(report_.state!=K70WifiNativeDmaStopped) {rc=stopLocked();if(rc!=kIOReturnSuccess)goto out;}
    if(report_.doorbell_references) {rc=kIOReturnBusy;goto out;}
    rc=platform_.verify_quiesced(platform_.cookie,&lease_);if(rc!=kIOReturnSuccess)goto out;
    if(data_held_) {
        rc=data_.acquireDeviceWrites(0,data_.bytes());if(rc!=kIOReturnSuccess)goto out;
        rc=data_.retireAfterQuiescence();if(rc!=kIOReturnSuccess)goto out;data_held_=false;
    }
    if(tables_held_) {rc=tables_.retireAfterQuiescence();if(rc!=kIOReturnSuccess)goto out;tables_held_=false;}
    rc=data_.release();if(rc!=kIOReturnSuccess)goto out;
    rc=tables_.release();if(rc!=kIOReturnSuccess)goto out;
    rc=platform_.release_lease(platform_.cookie,&lease_);if(rc!=kIOReturnSuccess)goto out;
    report_.lease_retained=false;report_.state=K70WifiNativeDmaReleased;
    report_.status=kIOReturnSuccess;arena_=nullptr;
out:
    if(rc!=kIOReturnSuccess)fail(rc);
    platform_.unlock(platform_.cookie,&lease_);platform_locked_=false;
    if(rc==kIOReturnSuccess)lease_={};
    leave();return rc;
}
