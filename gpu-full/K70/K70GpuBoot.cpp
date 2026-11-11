/* SPDX-License-Identifier: MIT */
#include "K70GpuBoot.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoGpuContext.h"
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_adreno_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *,
    NativeAdrenoRing *, const NativeAdrenoRingClock *, const NativeAdrenoTimer *,
    const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);

namespace {
uint32_t attempted;
/* These values match k70_gpu_hw_init, the native GPU page tables, and the
 * pinned SM8550 UBWC configuration. The client aperture ends at 2 GiB. */
const NativeAdrenoAbiDeviceInfo info = {
    0x43050a01u, 3u<<20, 0x1000000ull, 0x1fffffffff000ull,
    0x20000000ull, GpuApertureBytes-0x20000000ull,
    16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0
};
static_assert(GpuApertureBytes==0x80000000ull, "Bound advertised client VA to the live tables");
}

extern "C" IOReturn k70_gpu_boot(const K70GpuStartupConfig *config, K70GpuBootReport *report)
{
    if(!report || !config)return kIOReturnBadArgument;
    *report={};
    auto stop=[&](IOReturn status){report->status=status;return status;};
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&attempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return stop(kIOReturnBusy);
    report->stage=1;
    IOReturn status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if(status)return stop(status);
    const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    OSSafeReleaseNULL(name);
    if(!parent || parent->getClassSize()!=136)return stop(kIOReturnUnsupported);
    report->stage=2;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return stop(kIOReturnNoResources);
    native_adreno_accelerator_static_init();
    status=OSMetaClass::postModLoad(handle);
    if(status)return stop(status);
    if(native_adreno_accelerator_instance_size()!=136)return stop(kIOReturnInvalid);
    report->stage=3;
    status=k70_gpu_start_execution(config,&report->execution);
    if(status || !report->execution.execution_ready || !report->execution.zap_authenticated)
        return stop(status?status:kIOReturnNotReady);
    K70GpuExecution execution{};
    status=k70_gpu_execution(&execution);
    if(status)return stop(status);
    report->stage=4;
    status=native_adreno_publish(execution.vm,execution.submit,execution.ring,
        execution.clock,execution.timer,&info,32,40);
    if(status)return stop(status);
    report->stage=5;
    IOService *service=native_adreno_accelerator_allocate();
    if(!service)return stop(kIOReturnNoMemory);
    if(!service->init()){service->release();return stop(kIOReturnError);}
    service->setProperty("MetalPluginName","NativeAdrenoMetal");
    service->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice");
    if(!service->attach(IOService::getPlatform())){service->release();return stop(kIOReturnNotAttached);}
    service->registerService();
    report->stage=6;report->published=true;
    return stop(kIOReturnSuccess);
}
