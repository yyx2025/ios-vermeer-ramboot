// SPDX-License-Identifier: MIT
#ifndef K70_AUTOREFRESH_FIX_H
#define K70_AUTOREFRESH_FIX_H
#include <stdint.h>
// Deliberately accept only the observed single-interface vermeer/DPU9 command
// configuration. Count=1 is every TE; count=60 was measured at ~1 frame/s.
static inline bool k70_autorefresh_eligible(uint32_t version,uint32_t width,uint32_t height,
    uint32_t status,uint32_t timing,uint32_t counts,uint32_t command,
    uint32_t tear,uint32_t refresh,uint32_t peerTear,uint32_t peerRefresh)
{
    return version==0x90000000u && width==1440 && height==3200 &&
        status==0 && timing==0 && counts==0 && command==0 &&
        tear==1 && refresh==0x8000003cu && peerTear==0 && peerRefresh==0;
}
#endif
