/* SPDX-License-Identifier: MIT */
#ifndef K70_FRAME_HANDOFF_H
#define K70_FRAME_HANDOFF_H
#include "NativeGOPWire.h"
#include "K70FrameGate.h"
#include "vermeer_frame_scale.h"
#include "K70PixelContract.h"
#include <IOKit/IOMemoryDescriptor.h>
struct K70FrameInput {const uint8_t *base;K70PixelLayout layout;vframe_scale scale;};
/* Registration borrows the already-owned Normal-NC scanout VA and retains its descriptor. */
extern "C" IOReturn k70_frame_register(IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t,uint32_t,uintptr_t);
extern "C" unsigned k70_frame_offer(const K70FrameInput *,unsigned);
extern "C" bool k70_frame_cpu_begin(void);
extern "C" void k70_frame_cpu_end(void);
extern "C" IOReturn native_gop_handoff_call(uint32_t,task_t,const void *,size_t,void *,size_t);
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void);
#endif
