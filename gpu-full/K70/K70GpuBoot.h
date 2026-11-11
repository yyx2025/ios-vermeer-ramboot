/* SPDX-License-Identifier: MIT */
#ifndef K70_GPU_BOOT_H
#define K70_GPU_BOOT_H
#include "K70GpuStartup.h"
struct K70GpuBootReport {
    K70GpuStartupReport execution;
    IOReturn status;
    uint32_t stage;
    bool published;
};
/* Call once from a schedulable boot worker after IOKit and original prelinked
 * dependencies are available. The linker must supply the audited original
 * IOAcceleratorES bindings and late initializer; no weak fallback is accepted.
 * Failure never frees the execution owner's DMA resources. */
extern "C" IOReturn k70_gpu_boot(const K70GpuStartupConfig *, K70GpuBootReport *);
#endif
