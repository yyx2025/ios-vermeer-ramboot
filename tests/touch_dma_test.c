#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../xnu/vermeer/qcom-geni-spi/GeniSpi.c"
static uint32_t regs[0x4000/4];static uint8_t txdma[4096],rxdma[4096];
static unsigned mode,writes,cache_ops;
static uint32_t frd(void *c,uint32_t o){(void)c;assert(o<0x4000);return regs[o/4];}
static void fwr(void *c,uint32_t o,uint32_t v)
{
    (void)c;assert(o<0x4000);++writes;regs[o/4]=v;
    if(o==SE_DMA_TX_IRQ_CLR)regs[SE_DMA_TX_IRQ_STAT/4]&=~v;
    if(o==SE_DMA_RX_IRQ_CLR)regs[SE_DMA_RX_IRQ_STAT/4]&=~v;
    if(o==SE_GENI_M_IRQ_CLEAR)regs[SE_GENI_M_IRQ_STATUS/4]&=~v;
    if(o==SE_DMA_TX_LEN && mode==1) {
        memcpy(rxdma,txdma,v);regs[SE_DMA_TX_IRQ_STAT/4]=GENI_DMA_DONE;
        regs[SE_DMA_RX_IRQ_STAT/4]=GENI_DMA_DONE;
    }
    if(o==SE_DMA_TX_LEN && mode==2)regs[SE_DMA_TX_IRQ_STAT/4]=GENI_DMA_AHB_ERR;
    if(o==SE_GENI_M_CMD_CTRL_REG)regs[SE_GENI_M_IRQ_STATUS/4]=GENI_M_CMD_CANCEL_DONE;
}
static void pause_us(void *c,unsigned n){(void)c;(void)n;}
static void cache(void *c,void *p,size_t n){(void)c;assert(p && n<=4096);++cache_ops;}
static struct geni_spi make(void)
{
    memset(regs,0,sizeof(regs));writes=cache_ops=0;
    struct geni_spi s={0};s.se=(struct geni_io){0,frd,fwr,pause_us};s.use_dma=1;
    s.cur_speed_hz=4000000;s.cur_bpw=8;s.poll_limit=4;
    s.dma=(struct geni_dma){.clean=cache,.clean_inval=cache,.inval=cache,
        .tx_virt=txdma,.rx_virt=rxdma,.tx_phys=0x8c1234000ull,.rx_phys=0x8c1235000ull,.size=4096};return s;
}
int main(void)
{
    uint8_t tx[4096],rx[4096];for(unsigned i=0;i<4096;i++)tx[i]=(uint8_t)i;
    struct geni_spi s=make();mode=1;assert(!geni_spi_transfer(&s,tx,rx,4096,0));
    assert(!memcmp(tx,rx,4096) && cache_ops==3 && regs[SE_DMA_TX_PTR_H/4]==8);
    s=make();assert(geni_spi_transfer(&s,tx,rx,4097,0)==GENI_ERR_PARAM && !writes);
    s=make();mode=2;memset(rx,0xa5,sizeof(rx));assert(geni_spi_transfer(&s,tx,rx,20,0)==GENI_ERR_IO);
    for(unsigned i=0;i<4096;i++)assert(rx[i]==0xa5);
    assert(s.snap_valid && regs[SE_DMA_TX_FSM_RST/4]);
    s=make();mode=0;assert(geni_spi_transfer(&s,tx,rx,20,0)==GENI_ERR_TIMEOUT);
    for(unsigned i=0;i<4096;i++)assert(rx[i]==0xa5);
    puts("Actual SE DMA 64-bit addresses, transfer bound, cache ordering, bus error and timeout: PASS");
}
