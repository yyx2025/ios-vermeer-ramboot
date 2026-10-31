#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/vermeer/vermeer_ram_receive.h"
static struct vrx_state state;
static unsigned writes,replies,failures;
static uint64_t last_offset;
static int begin(void *c,uint64_t total,const uint8_t *sha)
{ (void)c;return !total || sha[0]!=0x42; }
static int data(void *c,uint64_t off,const void *p,size_t n)
{ (void)c;(void)p;assert(n<=VRX_CHUNK);writes++;last_offset=off;return 0; }
static int finish(void *c) { (void)c;return 0; }
static void reply(void *c,uint32_t seq,uint64_t received,int error)
{ (void)c;(void)seq;(void)received;replies++;if(error)failures++; }
static void put32(uint8_t *p,uint32_t n)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static void put64(uint8_t *p,uint64_t n)
{ for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t crc(const void *input,size_t n)
{
    const uint8_t *p=input;uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320u&(0u-(c&1u)));}
    return ~c;
}
static void header(uint8_t *h,uint32_t op,uint32_t seq,uint64_t off,uint32_t n,uint64_t total)
{
    memset(h,0,VRX_HEADER);memcpy(h,"VMRX0001",8);
    put32(h+8,op);put32(h+12,seq);put64(h+16,off);put32(h+24,n);put64(h+32,total);
    if(op==VRX_BEGIN)h[40]=0x42;
}
static void reset(void)
{
    struct vrx_ops ops={NULL,begin,data,finish,reply};vrx_init(&state,&ops);
    writes=replies=failures=0;last_offset=0;
}
int main(void)
{
    uint8_t h[VRX_HEADER],payload[5]={1,2,3,4,5};reset();
    header(h,VRX_BEGIN,0,0,0,5);
    for(unsigned i=0;i<VRX_HEADER;i++)assert(!vrx_feed(&state,h+i,1));
    assert(state.started && replies==1);
    header(h,VRX_DATA,1,0,5,5);put32(h+28,crc(payload,5));
    assert(!vrx_feed(&state,h,VRX_HEADER));
    for(unsigned i=0;i<5;i++)assert(!vrx_feed(&state,payload+i,1));
    assert(writes==1 && state.received==5);
    header(h,VRX_COMMIT,2,5,0,5);assert(!vrx_feed(&state,h,VRX_HEADER));
    assert(state.complete && replies==3 && !failures);
    assert(vrx_feed(&state,h,VRX_HEADER)<0); /* No duplicate commit or extra frames. */
    reset();header(h,VRX_BEGIN,0,0,0,5);assert(!vrx_feed(&state,h,VRX_HEADER));
    header(h,VRX_DATA,1,0,5,5);put32(h+28,crc(payload,5)^1);
    assert(!vrx_feed(&state,h,VRX_HEADER));assert(vrx_feed(&state,payload,5)<0 && !writes);
    reset();header(h,VRX_BEGIN,0,0,0,5);assert(!vrx_feed(&state,h,VRX_HEADER));
    header(h,VRX_DATA,2,0,5,5);assert(vrx_feed(&state,h,VRX_HEADER)<0 && !writes);
    reset();uint64_t high=UINT64_C(0x100000000)+9,total=high+5;
    header(h,VRX_BEGIN,0,0,0,total);assert(!vrx_feed(&state,h,VRX_HEADER));
    state.received=high;state.sequence=73;
    header(h,VRX_DATA,73,high,5,total);put32(h+28,crc(payload,5));
    assert(!vrx_feed(&state,h,VRX_HEADER));assert(!vrx_feed(&state,payload,5));
    assert(last_offset==high && state.received==total);
    reset();header(h,VRX_BEGIN,0,0,0,5);h[0]^=1;assert(vrx_feed(&state,h,VRX_HEADER)<0 && !writes);
    puts("RAM receiver fragmented frames, CRC-before-write, sequence, finalization and >4GiB offsets: PASS");
    return 0;
}
