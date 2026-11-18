/* SPDX-License-Identifier: MIT */
#ifndef K70_NATIVE_GOP_WIRE_H
#define K70_NATIVE_GOP_WIRE_H
#include <stdint.h>
#include <stddef.h>
enum {NGP_BGRX8888=1,NGP_VERSION=3,NGP_MAX_LAYERS=4};
enum {NGP_METHOD_MODE=0,NGP_METHOD_WAIT_SWAP=2,NGP_METHOD_COMPLETE_SWAP=3,NGP_METHOD_ARM=4};
enum {NGP_SOURCE_BGRA=0x42475241u,NGP_SOURCE_B3A8=0x62336138u};
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPWait {uint64_t timeout_ms;};
/* v3 snapshots isolate both b3a8 planes. Color starts at byte 0; alpha starts
 * at alpha_offset with alpha_stride bytes per row. Other versions are refused. */
struct NGPScaleLayer {
    uint64_t address,allocation_bytes,source_bytes,alpha_offset,alpha_bytes;
    uint32_t stride,alpha_stride,format,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph,reserved;
};
struct NGPScaleSwap {uint64_t sequence;uint32_t version,count;struct NGPScaleLayer layers[NGP_MAX_LAYERS];};
enum {NGP_QUIESCENT=1};
struct NGPComplete {uint64_t sequence;uint32_t status,flags;};
struct NGPArm {uint32_t version,reserved;};
struct NGPScaleConstants {
    uint32_t source_stride_words,target_stride_words,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;
    uint32_t format,alpha_offset,alpha_stride,previous_count;
    uint32_t previous_clip[12],reserved[4];
};
#ifdef __cplusplus
static_assert(sizeof(NGPMode)==40 && sizeof(NGPScaleLayer)==96 && sizeof(NGPScaleSwap)==400 && sizeof(NGPComplete)==16,"K70 v3 wire ABI");
static_assert(sizeof(NGPScaleConstants)==128,"K70 v3 Metal constants ABI");
static_assert(offsetof(NGPScaleLayer,alpha_offset)==24 && offsetof(NGPScaleLayer,stride)==40 &&
    offsetof(NGPScaleLayer,format)==48 && offsetof(NGPScaleLayer,reserved)==92,"K70 v3 layer offsets");
static_assert(offsetof(NGPScaleConstants,format)==48 && offsetof(NGPScaleConstants,previous_clip)==64 &&
    offsetof(NGPScaleConstants,reserved)==112,"K70 v3 shader offsets");
#endif
#endif
