/* SPDX-License-Identifier: MIT */
#ifndef K70_FRAME_HANDOFF_H
#define K70_FRAME_HANDOFF_H
#include "NativeGOPWire.h"
#include "K70FrameGate.h"
#include "vermeer_frame_scale.h"
#include <IOKit/IOMemoryDescriptor.h>
struct K70FrameInput {const uint8_t *base;uint64_t bytes;uint32_t stride;struct vframe_scale scale;};
/* Registration retains the bounded K70 scanout forever. CPU frame snapshots
 * are allocated once at ARM; they isolate GPU reads from producer reuse. */
extern "C" IOReturn k70_frame_register(IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t,uint32_t);
/* Called synchronously before SwapEnd acknowledges producer reuse. Returns
 * CPU only when no GPU owns scanout; GPU/blocked suppress CPU writes. Input
 * was clipped by vframe_scale_plan; this entry independently rechecks it. */
extern "C" unsigned k70_frame_offer(const K70FrameInput *,unsigned);
/* A CPU route is only a hint: ARM may race it. Hold this lease across every
 * panel write, including the initial clear. A successful begin requires end. */
extern "C" bool k70_frame_cpu_begin(void);
extern "C" void k70_frame_cpu_end(void);
extern "C" IOReturn native_gop_handoff_call(uint32_t,task_t,const void *,size_t,void *,size_t);
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void);
#endif
