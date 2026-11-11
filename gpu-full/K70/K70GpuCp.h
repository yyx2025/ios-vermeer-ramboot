/* SPDX-License-Identifier: MIT */
#ifndef K70_GPU_CP_H
#define K70_GPU_CP_H
#include "NativeAdrenoRing.h"
struct K70GpuHwIo {
    NativeAdrenoRingIo gpu;
    int (*write_gmu)(void *,uint32_t,uint32_t);
};
/* A740 branch of the pinned SM8550 hw_init. GX and OOB GPU_SET must be
 * held, with exclusive ownership. Every access failure stops the sequence. */
IOReturn k70_gpu_hw_init(const K70GpuHwIo *,uint64_t sqe_iova);
#endif
