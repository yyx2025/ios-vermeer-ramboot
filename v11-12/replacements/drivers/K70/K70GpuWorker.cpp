/* SPDX-License-Identifier: MIT */
/* The native display service schedules one GPU bring-up attempt after its
 * guarded panel mapping and CPU presentation path have become available. */
#include "K70GpuBoot.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
extern "C" IOReturn k70_aes_start(void);

#if !defined(K70_IOS16_IOKIT_ABI) || !K70_IOS16_IOKIT_ABI
#error K70GpuWorker requires the global iOS16 IOKit ABI rebuild
#endif

namespace {
uint32_t scheduled;
/* Exactly the bank/ASID/SMR assignment checked by k70_gpu_start_execution.
 * Level 1 is the lowest nonzero OPP in the pinned K70 GMU policy. */
const K70GpuStartupConfig config={1,1,2,0,2,0,1,1};
K70GpuBootReport retained_report{};

void gpuThread(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"k70-gpu-start");
    IOLog("K70 v11.12: startup worker entered\n");
    const IOReturn aes=k70_aes_start();
    IOLog("K70 AES: startup status 0x%x\n",aes);
    const IOReturn result=k70_gpu_boot(&config,&retained_report);
    IOLog("K70 GPU: startup stage %u status 0x%x published %u\n",
        retained_report.stage,result,retained_report.published?1:0);
    __atomic_store_n(&scheduled,2u,__ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
}

extern "C" IOReturn k70_gpu_start_worker(void)
{
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&scheduled,&expected,1u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return kIOReturnBusy;
    thread_t thread=THREAD_NULL;
    const kern_return_t result=kernel_thread_start(gpuThread,nullptr,&thread);
    if(result!=KERN_SUCCESS) {
        __atomic_store_n(&scheduled,0u,__ATOMIC_RELEASE);
        return kIOReturnNoResources;
    }
    thread_deallocate(thread);
    return kIOReturnSuccess;
}
