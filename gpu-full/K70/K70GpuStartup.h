/* SPDX-License-Identifier: MIT */
#ifndef K70_GPU_STARTUP_H
#define K70_GPU_STARTUP_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* A one-shot board startup entry, called after IOKit and native allocation are
 * available. prepare_gmu prepares GMU/HFI and powers GX; start_execution also
 * starts CP, authenticates signed ZAP and checks completed writes/fences.
 * It does not publish a Metal device or install sampling/periodic workers.
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
    K70GpuGmuPrepared, K70GpuSmmuActlr, K70GpuSqe, K70GpuHwInit,
    K70GpuRing, K70GpuCpInit, K70GpuFirstWrite, K70GpuZapMemory,
    K70GpuZapInit, K70GpuZapSetup, K70GpuZapCopy, K70GpuZapAuth,
    K70GpuSecureMode, K70GpuExecution
};
struct K70GpuStartupReport {
    uint32_t stage;
    IOReturn status;
    uint32_t platform_status, command_db_status, votes_status;
    uint32_t gmu_status, context_step, firmware_error, gx_status;
    uint32_t performance_level, bandwidth_level;
    uint32_t actlr, submitted_fence, observed_fence, scratch_value, scm_state;
    int64_t scm_x0;
    uint64_t scm_x1;
    bool zap_authenticated;
    bool gmu_ready, gpu_address_space_ready, execution_ready;
};
extern "C" IOReturn k70_gpu_prepare_gmu(const K70GpuStartupConfig *, K70GpuStartupReport *);
extern "C" IOReturn k70_gpu_start_execution(const K70GpuStartupConfig *, K70GpuStartupReport *);
#ifdef __cplusplus
class NativeAdrenoVm;
class NativeAdrenoSubmit;
class NativeAdrenoRing;
class NativeAdrenoTimer;
struct NativeAdrenoRingClock;
/* Borrowed live interfaces; only available after authentication and all
 * completion checks. They have permanent owner lifetime. No publication. */
struct K70GpuExecution {
    NativeAdrenoVm *vm;
    NativeAdrenoSubmit *submit;
    NativeAdrenoRing *ring;
    const NativeAdrenoRingClock *clock;
    const NativeAdrenoTimer *timer;
};
extern "C" IOReturn k70_gpu_execution(K70GpuExecution *);
#endif
#endif
