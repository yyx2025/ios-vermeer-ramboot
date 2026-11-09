/* CPU SHA256 instructions only: no MMU/cache, clock, SMMU or DMA changes. */
#include "ram_fastboot.h"
static int active;
static uint64_t saved_cpacr;
int rfb_sha_hardware_active(void) {return active;}
int rfb_sha_accel_begin(void)
{
    if(active)return 1;
    uint64_t level,isar,pfr;
    __asm__ volatile("mrs %0,CurrentEL":"=r"(level));
    if(level!=4)return 0;
    __asm__ volatile("mrs %0,id_aa64isar0_el1":"=r"(isar));
    __asm__ volatile("mrs %0,id_aa64pfr0_el1":"=r"(pfr));
    unsigned sha2=(unsigned)((isar>>12)&15);
    if((sha2!=1 && sha2!=2) || ((pfr>>16)&15)>1 || ((pfr>>20)&15)>1)return 0;
    __asm__ volatile("mrs %0,cpacr_el1":"=r"(saved_cpacr));
    uint64_t enabled=saved_cpacr|(UINT64_C(3)<<20),observed;
    __asm__ volatile("msr cpacr_el1,%0\n\tisb"::"r"(enabled):"memory");
    __asm__ volatile("mrs %0,cpacr_el1":"=r"(observed));
    if((observed&(UINT64_C(3)<<20))!=(UINT64_C(3)<<20)) {
        __asm__ volatile("msr cpacr_el1,%0\n\tisb"::"r"(saved_cpacr):"memory");
        return 0;
    }
    active=1;return 1;
}
void rfb_sha_accel_end(void)
{
    if(!active)return;
    active=0;
    __asm__ volatile("msr cpacr_el1,%0\n\tisb"::"r"(saved_cpacr):"memory");
}
