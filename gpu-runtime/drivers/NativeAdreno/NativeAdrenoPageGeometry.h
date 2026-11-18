/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PAGE_GEOMETRY_H
#define NATIVE_ADRENO_PAGE_GEOMETRY_H
#include <stddef.h>
#include <stdint.h>
/* CPU ownership granules and the GPU's 4 KiB translations are independent. */
static inline bool NativeAdrenoCpuGeometry(uint64_t tcr,uint64_t ttbr1,uint64_t sctlr,
    size_t compiled,size_t runtime,size_t map)
{
    const unsigned tg0=(unsigned)((tcr>>14)&3),tg1=(unsigned)((tcr>>30)&3);
    return compiled==runtime && compiled==map && (sctlr&1) &&
        (ttbr1&UINT64_C(0x0000fffffffff000)) &&
        ((compiled==4096 && tg0==0 && tg1==2) ||
         (compiled==16384 && tg0==2 && tg1==1));
}
static inline bool NativeAdrenoOwnedBytes(size_t bytes,size_t cpu_page,size_t *owned)
{
    if(!owned || !bytes || (bytes&4095) || (cpu_page!=4096 && cpu_page!=16384) ||
        bytes>SIZE_MAX-(cpu_page-1))return false;
    *owned=(bytes+cpu_page-1)&~(cpu_page-1);return true;
}
#endif
