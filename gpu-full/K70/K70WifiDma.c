/* SPDX-License-Identifier: MIT */
#include "K70WifiDma.h"
#include <string.h>
static int valid_direction(int direction) {return direction>=0 && direction<=2;}
int k70_wifi_dma_init(struct K70WifiDmaArena *a,void *cpu,uint64_t bus,size_t bytes,
    size_t page,void *cookie,int (*sync)(void *,void *,size_t,int))
{
    if(!a || !cpu || !bus || !bytes || !sync || (page!=4096 && page!=16384) ||
        (((uintptr_t)cpu|bus|bytes)&(page-1)) || bytes>UINTPTR_MAX-(uintptr_t)cpu ||
        bytes>UINT64_MAX-bus)return -1;
    memset(a,0,sizeof *a);a->cpu=cpu;a->bus=bus;a->bytes=bytes;a->page=page;
    a->cookie=cookie;a->sync_owned=sync;a->count=1;a->blocks[0].owned=bytes;
    return 0;
}
void k70_wifi_dma_quarantine(struct K70WifiDmaArena *a) {if(a)a->quarantined=1;}
void *k70_wifi_dma_alloc(struct K70WifiDmaArena *a,size_t n,uint64_t *bus)
{
    if(bus)*bus=0;
    if(!a || !a->cpu || a->quarantined || !bus || !n || n>SIZE_MAX-(a->page-1))return 0;
    const size_t owned=(n+a->page-1)&~(a->page-1);
    for(size_t i=0;i<a->count;++i) {
        struct K70WifiDmaBlock *b=&a->blocks[i];
        if(b->requested || b->owned<owned)continue;
        if(b->owned>owned) {
            if(a->count==K70WifiDmaMaxBlocks)continue;
            memmove(&a->blocks[i+2],&a->blocks[i+1],(a->count-i-1)*sizeof *b);
            a->blocks[i+1]=(struct K70WifiDmaBlock){b->offset+owned,b->owned-owned,0};
            ++a->count;b->owned=owned;
        }
        b->requested=n;a->used+=owned;
        void *cpu=a->cpu+b->offset;memset(cpu,0,owned);
        /* The whole CPU extent, including padding, is clean before any DMA publication. */
        if(a->sync_owned(a->cookie,cpu,owned,1)) {a->quarantined=1;return 0;}
        *bus=a->bus+b->offset;return cpu;
    }
    return 0;
}
static struct K70WifiDmaBlock *find(struct K70WifiDmaArena *a,size_t offset,size_t n)
{
    if(!n)return 0;
    for(size_t i=0;i<a->count;++i) {
        struct K70WifiDmaBlock *b=&a->blocks[i];
        if(b->requested && offset>=b->offset && offset-b->offset<=b->requested &&
            n<=b->requested-(offset-b->offset))return b;
    }
    return 0;
}
static int sync_range(struct K70WifiDmaArena *a,size_t offset,size_t n,int direction)
{
    if(!a || !a->cpu || a->quarantined || !valid_direction(direction) || !find(a,offset,n))return -1;
    const size_t mask=a->page-1,begin=offset&~mask,end=(offset+n+mask)&~mask;
    if(a->sync_owned(a->cookie,a->cpu+begin,end-begin,direction)) {a->quarantined=1;return -1;}
    return 0;
}
uint64_t k70_wifi_dma_map(struct K70WifiDmaArena *a,void *cpu,size_t n,int direction)
{
    if(!a || !cpu || (uintptr_t)cpu<(uintptr_t)a->cpu)return 0;
    const uintptr_t offset=(uintptr_t)cpu-(uintptr_t)a->cpu;
    return sync_range(a,offset,n,direction)?0:a->bus+offset;
}
int k70_wifi_dma_sync(struct K70WifiDmaArena *a,uint64_t bus,size_t n,int direction)
{
    if(!a || bus<a->bus || bus-a->bus>SIZE_MAX)return -1;
    return sync_range(a,(size_t)(bus-a->bus),n,direction);
}
int k70_wifi_dma_free(struct K70WifiDmaArena *a,size_t n,void *cpu,uint64_t bus)
{
    if(!a || !a->cpu || a->quarantined || !cpu || (uintptr_t)cpu<(uintptr_t)a->cpu)return -1;
    const size_t offset=(uintptr_t)cpu-(uintptr_t)a->cpu;
    for(size_t i=0;i<a->count;++i) {
        struct K70WifiDmaBlock *b=&a->blocks[i];
        if(b->offset!=offset || !b->requested)continue;
        if((n && n!=b->requested) || (bus && bus!=a->bus+offset))return -1;
        a->used-=b->owned;b->requested=0;
        if(i+1<a->count && !a->blocks[i+1].requested) {
            b->owned+=a->blocks[i+1].owned;
            memmove(&a->blocks[i+1],&a->blocks[i+2],(a->count-i-2)*sizeof *b);--a->count;
        }
        if(i && !a->blocks[i-1].requested) {
            a->blocks[i-1].owned+=b->owned;
            memmove(b,b+1,(a->count-i-1)*sizeof *b);--a->count;
        }
        return 0;
    }
    return -1;
}
