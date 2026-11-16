#include "K70BootLog.h"
/* SPDX-License-Identifier: MIT */
/* The native display service schedules one GPU bring-up attempt after its
 * guarded panel mapping and CPU presentation path have become available. */
#include "K70GpuBoot.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
extern "C" IOReturn k70_aes_start(void);
/* v11.31: this is the only entry point that actually runs during boot. The
 * K70StartupService personality carries IOResourceMatch=IOBSD, so it stays
 * unmatched until the root volume is live; by then the one-shot below has
 * long returned kIOReturnBusy. v11.27 put the console start in that service,
 * which is why the panel never showed a single "K70 USB:" line. */
extern "C" int k70_usb_console_start(void);
extern "C" int k70_usb_console_active(void);
/* v11.31: page accounting line every 15 s. Answers "why does it always run
 * out of memory and respring" with numbers instead of a guess. */
extern "C" int k70_mem_report_start(void);

#if !defined(K70_IOS16_IOKIT_ABI) || !K70_IOS16_IOKIT_ABI
#error K70GpuWorker requires the global iOS16 IOKit ABI rebuild
#endif

namespace {
uint32_t scheduled;
uint32_t replayed;
IOReturn retained_aes;
/* Exactly the bank/ASID/SMR assignment checked by k70_gpu_start_execution.
 * Level 1 is the lowest nonzero OPP in the pinned K70 GMU policy. */
const K70GpuStartupConfig config={1,1,2,0,2,0,1,1};
K70GpuBootReport retained_report{};

void gpuThread(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"k70-gpu-start");
    /* USB allocates its enqueue buffer on its boot-CPU worker. Let that
     * worker run before producing the one-shot diagnostics; ACTIVE already
     * has a buffer even when the host has not opened CDC yet. Bound the
     * wait so an unavailable USB port cannot prevent GPU/AES bring-up. */
    unsigned waited=0;
    while(!k70_usb_console_active() && waited<500) {IOSleep(10);++waited;}
    K70BootLog("K70 GPU: log buffer ready %u wait %ums\n",
        k70_usb_console_active()?1u:0u,waited*10);
    K70BootLog("K70 v11.12: startup worker entered\n");
    /* v11.31: GPU first.  v11.29 proved the AES provider trips __stack_chk_fail
     * right after postModLoad, and because AES used to run before k70_gpu_boot
     * the GPU six stages never executed.  Bring the accelerator up and publish
     * it first so the CA/Metal path exists and the panel reports its stage,
     * then attempt AES.  AES failure can no longer hide the GPU. */
    const IOReturn result=k70_gpu_boot(&config,&retained_report);
    K70BootLog("K70 GPU: startup stage %u status 0x%x published %u\n",
        retained_report.stage,result,retained_report.published?1:0);
    const IOReturn aes=k70_aes_start();
    retained_aes=aes;
    K70BootLog("K70 AES: startup status 0x%x\n",aes);
    __atomic_store_n(&scheduled,2u,__ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
}

/* Called outside the USB owner guard. The release/acquire on scheduled
 * publishes the completed reports to the boot-CPU USB worker. This also
 * recovers the final result if the bounded initial wait expired. */
extern "C" void k70_gpu_log_replay(void)
{
    if(__atomic_load_n(&scheduled,__ATOMIC_ACQUIRE)!=2 || !k70_usb_console_active())return;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&replayed,&expected,1u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    const auto &e=retained_report.execution;
    K70BootLog("K70 GPU: retained execution stage %u status 0x%x GMU %u context %u firmware %u GX %u\n",
        e.stage,e.status,e.gmu_status,e.context_step,e.firmware_error,e.gx_status);
    K70BootLog("K70 GPU: retained fence submitted %u observed %u scratch 0x%x ZAP %u ready %u\n",
        e.submitted_fence,e.observed_fence,e.scratch_value,
        e.zap_authenticated?1u:0u,e.execution_ready?1u:0u);
    K70BootLog("K70 GPU: retained startup stage %u status 0x%x published %u AES 0x%x\n",
        retained_report.stage,retained_report.status,retained_report.published?1u:0u,retained_aes);
}

extern "C" IOReturn k70_gpu_start_worker(void)
{
    /* v11.31: bring the USB console up before anything that can panic, and do
     * it here rather than in K70StartupService because this is the call
     * SweetFramebuffer::start() actually makes. Idempotent; it only spawns a
     * polling worker and never sleeps the caller. */
    const int usb=k70_usb_console_start();
    K70BootLog("K70 USB: console start returned %d\n",usb);
    K70BootLog("K70 MEM: reporter %d\n",k70_mem_report_start());

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
