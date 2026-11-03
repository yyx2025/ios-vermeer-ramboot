#define VERMEER_HOST_TEST 1
#include <assert.h>
#include <stdio.h>
#include "../xnu/vermeer/vermeer_ram_pump.h"
struct fake { uint64_t clock,bytes;unsigned steps,finish,fail,progress_every;int bad_state; };
static int step(void *p) {
    struct fake *f=p;f->steps++;
    if(f->progress_every && !(f->steps%f->progress_every))f->bytes+=UINT64_C(0x100000000);
    return f->fail && f->steps==f->fail?-1:0;
}
static int state(void *p) {struct fake *f=p;return f->bad_state?-1:(f->finish && f->steps>=f->finish);}
static uint64_t progress(void *p) {return ((struct fake *)p)->bytes;}
static uint64_t now(void *p) {return ((struct fake *)p)->clock;}
static void idle(void *p) {((struct fake *)p)->clock++;}
int main(void) {
    struct fake f={.finish=2};struct vrp_ops ops={&f,step,state,progress,now,idle};
    assert(vrp_wait(&ops,5)==VRP_COMPLETE && f.steps==2);
    f=(struct fake){0};assert(vrp_wait(&ops,5)==VRP_TIMEOUT && f.steps==6);
    f=(struct fake){.finish=100,.progress_every=3};
    assert(vrp_wait(&ops,5)==VRP_COMPLETE && f.bytes>UINT32_MAX);
    f=(struct fake){.finish=9,.fail=3};assert(vrp_wait(&ops,5)==VRP_FAILED && f.steps==3);
    f=(struct fake){.bad_state=1};assert(vrp_wait(&ops,5)==VRP_FAILED && f.steps==1);
    f=(struct fake){.clock=UINT64_MAX-2};assert(vrp_wait(&ops,5)==VRP_TIMEOUT && f.steps==6);
    f=(struct fake){0};assert(vrp_wait(&ops,0)==VRP_TIMEOUT && f.steps==0);
    puts("Actual bootstrap pump: no worker needed, idle timeout, >4GiB progress, faults and counter wrap: PASS");
}
