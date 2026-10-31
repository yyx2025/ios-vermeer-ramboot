/* Malformed FDTs and reserved ranges must fail before RAM can be selected. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/ram_guard.h"

static uint8_t f[4096];
static unsigned pos;
static unsigned split_memory;
static const char names[] = "#address-cells\0#size-cells\0reg\0ranges\0";
static void be32(uint8_t *p, uint32_t v)
{ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void be64(uint8_t *p, uint64_t v)
{ be32(p,v>>32); be32(p+4,v); }
static void word(uint32_t v) { be32(f+pos,v); pos+=4; }
static void node(const char *name)
{ word(1); size_t n=strlen(name)+1; memcpy(f+pos,name,n); pos=(pos+n+3)&~3; }
static void prop(unsigned off, const uint8_t *value, unsigned n)
{ word(3); word(n); word(off); memcpy(f+pos,value,n); pos=(pos+n+3)&~3; }
static void cells(unsigned off)
{ uint8_t value[4]; be32(value,2); prop(off,value,4); }
static unsigned fixture(uint64_t reserved, uint64_t reserved_size)
{
    memset(f,0,sizeof(f));pos=72;
    if (reserved_size) { be64(f+40,reserved);be64(f+48,reserved_size); }
    node("");cells(0);cells(15);
    node("memory@80000000");
    uint8_t banks[48];
    be64(banks,0x80000000);be64(banks+8,0x70000000);
    be64(banks+16,0x880000000ULL);be64(banks+24,0x240000000ULL);
    if (split_memory) {
        be64(banks+16,0x8c0000000ULL);be64(banks+24,0xc0000000ULL);
        be64(banks+32,0x980000000ULL+(split_memory==2 ? 1 : 0));be64(banks+40,0x180000000ULL);
    }
    prop(27,banks,split_memory ? 48 : 32);word(2);
    node("reserved-memory");cells(0);cells(15);prop(31,banks,0);
    node("firmware@a00000000");be64(banks,0xa00000000ULL);be64(banks+8,0x10000);
    prop(27,banks,16);word(2);word(2);word(2);word(9);
    unsigned strings=pos;
    memcpy(f+pos,names,sizeof(names));pos+=sizeof(names);
    be32(f,0xd00dfeed);be32(f+4,pos);be32(f+8,72);be32(f+12,strings);be32(f+16,40);
    be32(f+20,17);be32(f+24,16);be32(f+32,sizeof(names));be32(f+36,strings-72);
    return pos;
}
int main(void)
{
    struct rg_layout layout;
    unsigned n=fixture(0,0);
    assert(ram_guard_collect(f,n,HIGH_RAM_LO,HIGH_RAM_HI,&layout)==0);
    assert(layout.count==2 && layout.bank[1].base==0x880000000ULL && layout.bank[1].size==0x240000000ULL);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_HI)==0);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_LO+0x100000000ULL)==0);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_LO+0x180000000ULL)==16);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_LO+0x280000000ULL)!=0);
    for (unsigned length=0;length<n;length++) assert(ram_guard(f,length,HIGH_RAM_LO,HIGH_RAM_HI)!=0);
    n=fixture(HIGH_RAM_LO+0x10000,0x4000);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_HI)==4);
    n=fixture(UINT64_MAX-0xff,0x1000);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_HI)==4);
    n=fixture(0,0);be32(f+36,0xffffffff);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_HI)!=0);
    split_memory=1;n=fixture(0,0);
    assert(ram_guard_collect(f,n,HIGH_RAM_LO,HIGH_RAM_LO+0x100000000ULL,&layout)==0 && layout.count==3);
    split_memory=2;n=fixture(0,0);
    assert(ram_guard(f,n,HIGH_RAM_LO,HIGH_RAM_LO+0x100000000ULL)==18);
    struct rg_layout unordered={.count=3,.bank={{0x980000000ULL,0x180000000ULL},{0x80000000,0x70000000},{0x8c0000000ULL,0xc0000000ULL}}};
    assert(rg_covers(&unordered,HIGH_RAM_LO,0xb00000000ULL));
    assert(!rg_covers(&unordered,HIGH_RAM_LO,0xb00000001ULL));
    puts("RAM layout: adjacent/unsorted banks, one-byte gap, fixed reservations, overflow and truncation: PASS");
}
