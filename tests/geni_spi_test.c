#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/vermeer/vermeer_geni_spi.h"
struct mock {
    unsigned n,sent,read,qhead,qtail,active,done,writes,pauses,cancel;
    unsigned fault;
    uint32_t fifo[32];
};
static uint32_t read_reg(void *ctx,unsigned r)
{
    struct mock *m=ctx;
    if(r==VG_STATUS)return m->active;
    if(r==VG_IRQ) {
        if(m->fault==5 && m->n && m->pauses<4000)return 0;
        return m->fault==1 && m->active?8:m->done;
    }
    if(r==VG_TX_COUNT)return m->fault==2?33:0;
    if(r==VG_RX_COUNT) {
        unsigned words=m->qtail-m->qhead;
        if(m->fault==3 && m->active)return 33;
        if(m->sent==m->n && words)return words|0x80000000u|((m->n%4)<<28);
        return words;
    }
    if(r==VG_RX){assert(m->qhead<m->qtail);m->read++;return m->fifo[m->qhead++%32];}
    return 0;
}
static void write_reg(void *ctx,unsigned r,uint32_t v)
{
    struct mock *m=ctx;m->writes++;
    if(r==VG_CLEAR)m->done&=~v;
    if(r==0x26c)m->n=v;
    if(r==VG_CTRL){m->cancel++;m->active=0;}
    if(r==VG_CMD) {
        unsigned op=v>>27;
        if(op==8 || op==9){m->done=1;m->active=0;}
        else {assert(op==3 && (v&4));m->active=1;}
    }
    if(r==VG_TX) {
        assert(m->active && m->qtail-m->qhead<32);
        m->fifo[m->qtail++%32]=v;
        unsigned remaining=m->n-m->sent;m->sent+=remaining<4?remaining:4;
        if(m->sent==m->n && m->fault!=4){m->active=0;m->done=1;}
    }
}
static void pause_bus(void *ctx){((struct mock *)ctx)->pauses++;}
static struct vg_bus bus(struct mock *m)
{ return (struct vg_bus){.ctx=m,.read=read_reg,.write=write_reg,.pause=pause_bus,.tx_depth=32,.rx_depth=32}; }
int main(void)
{
    assert(vg_pack(0)==0x7f8fe && vg_pack(2)==0xffefe);
    uint8_t tx[1032],rx[1032];
    for(unsigned i=0;i<sizeof(tx);i++)tx[i]=(uint8_t)(i*17+9);
    for(unsigned n=1;n<=sizeof(tx);n++) {
        struct mock m={0};struct vg_bus b=bus(&m);memset(rx,0,sizeof(rx));
        assert(vg_transfer(&b,tx,rx,n));assert(!memcmp(tx,rx,n));
        assert(!m.active && !b.failed && !m.cancel);
        assert(m.read==(n+3)/4 && m.pauses<100);
    }
    for(unsigned fault=1;fault<=4;fault++) {
        struct mock m={0};struct vg_bus b=bus(&m);m.fault=fault;
        /* Stale impossible TX count is refused before asserting CS. */
        assert(!vg_transfer(&b,tx,rx,131));
        if(fault!=2) {
            assert(b.failed);unsigned writes=m.writes;
            assert(!vg_transfer(&b,tx,rx,131));assert(m.writes==writes);
        }
        assert(m.pauses<=vg_transfer_polls(131)+200);
    }
    struct mock m={0};struct vg_bus b=bus(&m);
    assert(!vg_transfer(&b,tx,rx,1033) && m.writes==0);
    assert(!vg_transfer(&b,tx,rx,0) && m.writes==0);
    assert(!vg_transfer(NULL,tx,rx,1));
    b.tx_depth=1;assert(!vg_transfer(&b,tx,rx,1) && m.writes==0);
    assert(vg_transfer_polls(1032)==26300 && vg_transfer_polls(8)<1000);
    assert(vg_transfer_polls(1033)==0);
    struct mock slow={0};slow.fault=5;struct vg_bus sb=bus(&slow);
    assert(vg_transfer(&sb,tx,rx,1032) && !memcmp(tx,rx,1032));
    assert(slow.pauses>=4000 && !sb.failed);
    puts("GENI FIFO 1..1032 byte tails, errors, impossible counts, timeout and retired transport: PASS");
}
