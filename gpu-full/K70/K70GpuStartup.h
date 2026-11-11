/* SPDX-License-Identifier: MIT */
#ifndef K70_GPU_STARTUP_H
#define K70_GPU_STARTUP_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* A one-shot board startup entry, called after IOKit and native allocation are
 * available. This prepares GMU/HFI and powers GX; it does not publish a GPU or
 * a Metal device. No phone sampling, periodic worker or boot hook is installed.
 * The startup owner retains all MMIO, tables and DMA memory after any attempt.
 * A reset/quiescence implementation is required before retry or teardown. */
struct K70GpuStartupConfig {
    uint32_t gmu_bank, gmu_asid, gmu_slot;
    uint32_t gpu_bank, gpu_asid, gpu_slot0, gpu_slot1;
    uint32_t performance_level;
};
enum K70GpuStartupStage : uint32_t {
    K70GpuValidate=1, K70GpuPower, K70GpuClocks, K70GpuIdle,
    K70GpuCommandDb, K70GpuGmuContext, K70GpuFirmware,
    K70GpuHfi, K70GpuFrequency, K70GpuGx, K70GpuAddressSpace,
    K70GpuGmuPrepared
};
struct K70GpuStartupReport {
    uint32_t stage;
    IOReturn status;
    uint32_t platform_status, command_db_status, votes_status;
    uint32_t gmu_status, context_step, firmware_error, gx_status;
    uint32_t performance_level, bandwidth_level;
    bool gmu_ready, gpu_address_space_ready, execution_ready;
};
extern "C" IOReturn k70_gpu_prepare_gmu(const K70GpuStartupConfig *, K70GpuStartupReport *);
#endif
