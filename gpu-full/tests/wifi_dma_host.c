#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "K70WifiDma.h"
static struct K70WifiDmaArena arena;
static unsigned char memory[1024*1024+16384];
static size_t page, calls;
static int fail;
static unsigned char *base;
static int sync_pages(void *cookie,void *cpu,size_t bytes,int direction) {
    assert(cookie==&arena && cpu>= (void *)base && (unsigned char *)cpu+bytes<=base+1024*1024);
    assert(!(((uintptr_t)cpu|bytes)&(page-1)) && bytes && direction>=0 && direction<=2);
    calls++;return fail;
}
static void invariant(void) {
    size_t end=0,used=0;
    assert(arena.count && arena.count<=K70WifiDmaMaxBlocks);
    for(size_t i=0;i<arena.count;i++) {
        const struct K70WifiDmaBlock *b=&arena.blocks[i];
        assert(b->offset==end && b->owned && !((b->offset|b->owned)&(page-1)) && b->requested<=b->owned);
        if(b->requested) {
            used+=b->owned;
            size_t align=page;while(align<b->requested)align*=2;
            assert(!((arena.bus+b->offset)&(align-1)));
        }
        if(i && !b->requested)assert(arena.blocks[i-1].requested);
        end+=b->owned;
    }
    assert(end==arena.bytes && used==arena.used);
}
static void reset(void) {
    fail=0;calls=0;memset(base,0xcc,1024*1024);
    assert(!k70_wifi_dma_init(&arena,base,0x40000000,1024*1024,page,&arena,sync_pages));
}
int main(int argc,char **argv) {
    assert(argc==2);page=strtoul(argv[1],NULL,0);assert(page==4096||page==16384);
    base=(unsigned char *)(((uintptr_t)memory+page-1)&~(uintptr_t)(page-1));
    assert(k70_wifi_dma_init(&arena,base+1,0x40000000,1024*1024,page,&arena,sync_pages));
    assert(k70_wifi_dma_init(&arena,base,UINT64_MAX-page+1,1024*1024,page,&arena,sync_pages));
    assert(k70_wifi_dma_init(&arena,base,0x40000000,1024*1024,8192,&arena,sync_pages));
    reset();uint64_t ba=9,bb=9;
    assert(!k70_wifi_dma_alloc(&arena,0,&ba)&&!ba);
    assert(!k70_wifi_dma_alloc(&arena,SIZE_MAX,&ba)&&!ba);
    unsigned char *a=k70_wifi_dma_alloc(&arena,1,&ba),*b=k70_wifi_dma_alloc(&arena,page+1,&bb);
    assert(calls==2);
    assert(a==base && b==base+2*page && ba==0x40000000 && bb==ba+2*page && arena.used==3*page);
    for(size_t i=0;i<page;i++)assert(a[i]==0);
    for(size_t i=0;i<2*page;i++)assert(b[i]==0);
    for(size_t i=page;i<2*page;i++)assert(base[i]==0xcc);
    assert(k70_wifi_dma_map(&arena,a,1,1)==ba);
    assert(!k70_wifi_dma_map(&arena,a,2,1)); /* requested length, not padded extent */
    assert(!k70_wifi_dma_map(&arena,b+page,2,2));
    assert(k70_wifi_dma_map(&arena,b+page,1,2)==bb+page);
    assert(k70_wifi_dma_sync(&arena,bb-1,2,0)); /* cannot cross allocations */
    assert(!k70_wifi_dma_sync(&arena,bb,page+1,0));
    assert(k70_wifi_dma_sync(&arena,bb,page+1,3));
    assert(k70_wifi_dma_free(&arena,2,a,ba));
    assert(k70_wifi_dma_free(&arena,1,a,ba+page));
    assert(!k70_wifi_dma_free(&arena,0,a,0));
    assert(k70_wifi_dma_free(&arena,0,a,0));
    assert(!k70_wifi_dma_free(&arena,0,b,0));invariant();assert(arena.count==1&&!arena.used);
    /* Exercise fragmentation, split/merge and exact base resolution with repeatable inputs. */
    void *ptr[48]={0};uint64_t bus[48]={0};size_t lengths[48]={0};unsigned state=0x70;
    for(unsigned i=0;i<50000;i++) {
        state=state*1664525u+1013904223u;unsigned slot=state%48;
        if(ptr[slot]) {assert(!k70_wifi_dma_free(&arena,lengths[slot],ptr[slot],bus[slot]));ptr[slot]=0;}
        else {lengths[slot]=1+((state>>8)%(5*page));ptr[slot]=k70_wifi_dma_alloc(&arena,lengths[slot],&bus[slot]);}
        invariant();
    }
    for(unsigned i=0;i<48;i++)if(ptr[i])assert(!k70_wifi_dma_free(&arena,0,ptr[i],0));
    invariant();assert(arena.count==1&&!arena.used);
    /* Large firmware after a small allocation, with a deliberately non-natural
     * arena base: align the absolute bus address, not just the offset. */
    reset();
    assert(!k70_wifi_dma_init(&arena,base,0x40000000+page,1024*1024,page,&arena,sync_pages));
    a=k70_wifi_dma_alloc(&arena,1,&ba);
    b=k70_wifi_dma_alloc(&arena,384*1024,&bb);
    assert(a && b && !(bb&((512*1024)-1)) && bb>=ba+page);
    invariant();
    assert(!k70_wifi_dma_free(&arena,0,a,0));
    assert(!k70_wifi_dma_free(&arena,0,b,0));
    invariant();assert(arena.count==1 && !arena.used);
    reset();a=k70_wifi_dma_alloc(&arena,17,&ba);assert(a);fail=1;
    assert(k70_wifi_dma_sync(&arena,ba,17,2));assert(arena.quarantined);
    assert(k70_wifi_dma_free(&arena,17,a,ba));assert(!k70_wifi_dma_alloc(&arena,17,&bb)&&!bb);
    reset();fail=1;assert(!k70_wifi_dma_alloc(&arena,17,&ba)&&!ba);
    assert(arena.quarantined&&arena.used==page); /* uncertain DMA/cache state retains storage */
    printf("Wi-Fi DMA %zu-byte CPU pages: bounds, cache extents, 50000 split/merge operations, quarantine PASS\n",page);
    return 0;
}
