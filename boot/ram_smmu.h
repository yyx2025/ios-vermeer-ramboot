/* Read-only pre-XNU policy for the exact K70 SMMUv2 geometry.
 * Both sCR0 profiles retain CLIENTPD=0, USFCFG=1, EXIDENABLE=0 and
 * the same architected stream controls. The clear direct ABL RAM-boot
 * photo on 2026-11-08 shows 0x00270406; 0x00280406 is the older
 * sample. Their upper-field difference is NOT masked generally: accept
 * only these two values (plus fault-reporting bits), then require one
 * matching SID40 route and identity context before starting USB DMA.
 * This does not modify the linked XNU USB policy or any SMMU register.
 */
#ifndef VERMEER_RAM_SMMU_H
#define VERMEER_RAM_SMMU_H
#include <stdint.h>
static inline int ram_smmu_geometry(uint32_t id0,uint32_t id1,uint32_t id2)
{
    return id0==0x4c017e69u && id1==0x60000046u && id2==0x5111u;
}
static inline int ram_smmu_control(uint32_t control)
{
    const uint32_t fault_reporting=0x36u;
    uint32_t pinned=control&~fault_reporting;
    return pinned==(0x00280406u&~fault_reporting) ||
           pinned==(0x00270406u&~fault_reporting);
}
#endif
