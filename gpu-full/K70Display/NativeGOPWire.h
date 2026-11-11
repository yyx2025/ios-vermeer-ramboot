/* SPDX-License-Identifier: MIT */
#ifndef K70_NATIVE_GOP_WIRE_H
#define K70_NATIVE_GOP_WIRE_H
#include <stdint.h>
enum {NGP_BGRX8888=1,NGP_VERSION=2,NGP_MAX_LAYERS=4};
enum {NGP_METHOD_MODE=0,NGP_METHOD_WAIT_SWAP=2,NGP_METHOD_COMPLETE_SWAP=3,NGP_METHOD_ARM=4};
/* Version 2 is explicit: the former single-frame, same-size ABI is refused. */
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPWait {uint64_t timeout_ms;};
struct NGPScaleLayer {
    uint64_t address,allocation_bytes,source_bytes;
    uint32_t stride,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;
    uint32_t reserved;
};
struct NGPScaleSwap {uint64_t sequence;uint32_t version,count;struct NGPScaleLayer layers[NGP_MAX_LAYERS];};
enum {NGP_QUIESCENT=1};
/* Every completion needs QUIESCENT to return ownership, including status 0.
 * Without it, scanout and snapshots stay quarantined. A timeout/error alone
 * never proves that DMA has stopped. Unknown flags do not retire a frame. */
struct NGPComplete {uint64_t sequence;uint32_t status,flags;};
struct NGPArm {uint32_t version,reserved;};
struct NGPScaleConstants {uint32_t source_stride_words,target_stride_words,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;};
#ifdef __cplusplus
static_assert(sizeof(NGPMode)==40 && sizeof(NGPScaleLayer)==72 && sizeof(NGPScaleSwap)==304 && sizeof(NGPComplete)==16,"K70 scale wire ABI");
static_assert(sizeof(NGPScaleConstants)==48,"Metal scale constants ABI");
#endif
#endif
