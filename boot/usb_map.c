/* Read-only USB/SMMU state before XNU. Never changes controller registers. */
#include "console.h"
#include "ram_guard.h"
void usb_map_dump(uintptr_t dtb);
static uint32_t usb_rd(uintptr_t base,unsigned offset)
{
    uint32_t v=*(volatile const uint32_t *)(base+offset);
    __asm__ volatile("dmb oshld" ::: "memory");return v;
}
void usb_map_dump(uintptr_t dtb)
{
    if (dtb<0xb5d00000 || dtb>=0xb5f00000 || (dtb&7) ||
        ram_guard((const uint8_t *)dtb,0xb5f00000-dtb,HIGH_RAM_LO,HIGH_RAM_HI)) {
        con_str("RESULT","REFUSED: live FDT invalid");return;
    }
    con_str("USB MAP","register reads only; no XNU jump");
    uintptr_t dwc=0xa600000,smmu=0x15000000;
    con_hex("DWCID",usb_rd(dwc,0xc120));
    con_hex("GCTL",usb_rd(dwc,0xc110));
    con_hex("DCTL",usb_rd(dwc,0xc704));
    uint32_t id0=usb_rd(smmu,0x20),id1=usb_rd(smmu,0x24),id2=usb_rd(smmu,0x28);
    con_hex("SMMUID0",id0);con_hex("SMMUID1",id1);con_hex("SMMUID2",id2);
    con_hex("SMMUCTL",usb_rd(smmu,0));
    con_hex("EXPECT0",0x4c017e69);con_hex("EXPECT1",0x60000046);con_hex("EXPECT2",0x5111);
    unsigned count=id0&255,matches=0;
    con_hex("SMCOUNT",count);
    if (!count || count>128 || !(id0&(1u<<27))) {
        con_str("RESULT","Unsupported SMMU stream table; reads stopped");return;
    }
    for (unsigned i=0;i<count;i++) {
        uint32_t smr=usb_rd(smmu,0x800+4*i);
        if (!(smr>>31) || ((smr^0x40)&~((smr>>16)&0x7fff)&0xffff))continue;
        uint32_t route=usb_rd(smmu,0xc00+4*i);
        matches++;
        con_hex("MATCH",i);con_hex("SMR",smr);con_hex("S2CR",route);
        if (((route>>16)&3)==0) {
            unsigned cb=route&255;
            /* This diagnostic only reads known v2 layout; never trusts a
             * context index outside the known 70-bank mapping aperture.
             */
            if (id1!=0x60000046 || cb>=70)continue;
            con_hex("CBAR",usb_rd(smmu,0x1000+4*cb));
            con_hex("CBSCTLR",usb_rd(smmu,0x80000+0x1000*cb));
        }
        if (matches==4)break; /* Bounded screen output even on broad matches. */
    }
    con_hex("MATCHES",matches);
    con_str("RESULT","USB STATE READ; no USB mapping changed");
}
