/* SPDX-License-Identifier: MIT */
#pragma once
#include "../K70Display/NativeGOPWire.h"
/* Shared by the real presenter and malformed-input fixtures. All reads made
 * by the shader are checked independently of the kernel's snapshot writer. */
static inline bool k70_wire_layer_valid(const NGPScaleLayer &f,uint64_t page,uint32_t width,uint32_t height) {
    if(!page || (page&(page-1)) || !f.address || (f.address&(page-1)) ||
       !f.allocation_bytes || (f.allocation_bytes&(page-1)) || f.allocation_bytes>(256u<<20) ||
       f.allocation_bytes>UINT64_MAX-f.address || !f.source_bytes || f.source_bytes>f.allocation_bytes ||
       !f.stride || (f.stride&3) || f.lw!=1125 || f.lh!=2436 || f.pw!=width || f.ph!=height ||
       width!=1440 || height!=3200 || f.reserved || f.x0>=f.x1 || f.y0>=f.y1 || f.x1>f.pw || f.y1>f.ph ||
       f.lx>uint64_t(f.x0)*f.lw/f.pw || f.ly>uint64_t(f.y0)*f.lh/f.ph)return false;
    const uint64_t sx=uint64_t(f.x1-1)*f.lw/f.pw-f.lx,sy=uint64_t(f.y1-1)*f.lh/f.ph-f.ly;
    if((sx+1)*4>f.stride || sy*f.stride+(sx+1)*4>f.source_bytes)return false;
    if(f.format==NGP_SOURCE_BGRA)return !f.alpha_offset && !f.alpha_bytes && !f.alpha_stride;
    if(f.format!=NGP_SOURCE_B3A8 || !f.alpha_bytes || !f.alpha_stride || (f.alpha_offset&15) ||
       f.alpha_offset<f.source_bytes || f.alpha_offset>UINT32_MAX || f.alpha_offset>f.allocation_bytes ||
       f.alpha_bytes>f.allocation_bytes-f.alpha_offset || sx+1>f.alpha_stride ||
       sy*f.alpha_stride+sx+1>f.alpha_bytes)return false;
    return true;
}
