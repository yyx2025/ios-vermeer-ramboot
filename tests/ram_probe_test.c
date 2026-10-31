#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/ram_probe.h"
#define COUNT 64u
static uint64_t memory[COUNT], original[COUNT], saved[COUNT];
struct state { int alias, stuck; unsigned writes; };
static unsigned index_of(uint64_t a)
{
    for (unsigned i = 0; i < COUNT; i++)
        if (a == rp_address(UINT64_C(0x8c0000000), RP_MAX_BYTES, COUNT, i)) return i;
    assert(0); return 0;
}
static uint64_t read_word(void *ctx, uint64_t a)
{
    struct state *s = ctx; unsigned i = index_of(a);
    if (s->alias && i == COUNT-1) i = 0;
    return memory[i];
}
static void write_word(void *ctx, uint64_t a, uint64_t value)
{
    struct state *s = ctx; unsigned i = index_of(a); s->writes++;
    if (s->alias && i == COUNT-1) i = 0;
    if (!(s->stuck && i == 7)) memory[i] = value;
}
static void barrier(void *ctx) { (void)ctx; }
static void init(void)
{
    for (unsigned i = 0; i < COUNT; i++) memory[i] = UINT64_C(0xf00d0000) + i;
    memcpy(original, memory, sizeof(memory));
}
int main(void)
{
    struct state s = {0,0,0};
    struct rp_io io = {&s,read_word,write_word,barrier};
    struct rp_result result;
    init();
    assert(!rp_run(&io,UINT64_C(0x8c0000000),RP_MAX_BYTES,COUNT,saved,&result));
    assert(result.restored && !memcmp(memory,original,sizeof(memory)));
    s.alias=1; s.writes=0; init();
    assert(rp_run(&io,UINT64_C(0x8c0000000),RP_MAX_BYTES,COUNT,saved,&result)==2);
    assert(result.phase==1 && result.restored && !memcmp(memory,original,sizeof(memory)));
    s.alias=0; s.stuck=1; s.writes=0; init();
    assert(rp_run(&io,UINT64_C(0x8c0000000),RP_MAX_BYTES,COUNT,saved,&result)==2);
    assert(result.restored && !memcmp(memory,original,sizeof(memory)));
    s.writes=0;
    assert(rp_run(&io,UINT64_MAX-7,RP_MAX_BYTES,COUNT,saved,&result)==1 && !s.writes);
    assert(rp_run(&io,UINT64_C(0x8c0000000),RP_MAX_BYTES,1,saved,&result)==1 && !s.writes);
    assert(rp_address(UINT64_C(0x8c0000000),RP_MAX_BYTES,RP_MAX_SAMPLES,RP_MAX_SAMPLES-1)==UINT64_C(0xabffffff8));
    puts("Sparse write patterns, alias/stuck detection, original restoration and bounds: PASS");
    return 0;
}
