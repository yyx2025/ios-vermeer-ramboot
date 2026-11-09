#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
static void delay(unsigned us){(void)us;}
static vm_offset_t ml_io_map(vm_offset_t p,size_t n){(void)n;return p;}
#define K70_RPMH_HOST_TEST 1
#include "../xnu/vermeer/k70_rpmh.h"
static uint32_t regs[KR_RSC_SIZE/4];static unsigned writes,mode;
static uint32_t kr_rd(vm_offset_t b,unsigned o){(void)b;assert(o<KR_RSC_SIZE);return regs[o/4];}
static void kr_wr(vm_offset_t b,unsigned o,uint32_t v)
{
    (void)b;assert(o<KR_RSC_SIZE);++writes;regs[o/4]=v;
    if(o==KR_TCS_OFFSET+kr.r->irq_clear)regs[(KR_TCS_OFFSET+kr.r->irq_status)/4]&=~v;
    if(mode && o==kr_treg(0,kr.r->control) && (v&KR_AMC_TRIGGER))
        regs[(KR_TCS_OFFSET+kr.r->irq_status)/4]|=1;
}
static void fresh(void)
{
    memset(&kr,0,sizeof(kr));memset(regs,0,sizeof(regs));kr.r=&kr_v30;kr.rsc=1;writes=mode=0;
    for(unsigned t=0;t<3;t++)regs[kr_treg(t,kr.r->status)/4]=1;
}
int main(void)
{
    uint32_t addr[3]={0x10000,0x10008,0x10004},data[3]={1800,7,1};
    fresh();assert(!kr_send(addr,data,4) && !writes);
    for(unsigned t=0;t<3;t++)regs[kr_treg(t,kr.r->status)/4]=0;
    assert(!kr_send(addr,data,3) && !writes);
    fresh();mode=1;assert(kr_send(addr,data,3));
    assert(!regs[kr_treg(0,kr.r->cmd_enable)/4] && kr.done_how==1);
    fresh();regs[(KR_TCS_OFFSET+kr.r->irq_status)/4]=1; /* stale completion */
    assert(!kr_send(addr,data,3));assert(kr.error && regs[kr_treg(0,kr.r->cmd_enable)/4]==7);
    unsigned count=writes;assert(!kr_send(addr,data,3) && writes==count);
    fresh();assert(!kr_ldo_on(2,"ldob12",1800));assert(!kr_ldo_on(1,"ldob12",3200));
    assert(!kr_ldo_on(0,"ldob13",3200) && !writes);
    assert(kr_init());
    puts("RPMh idle ownership, stale completion, timeout retention and exact rail gates: PASS");
}
