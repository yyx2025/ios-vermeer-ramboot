#include "K70GpuCp.h"
#include "NativeAdrenoA740HwInit.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Io {std::map<uint32_t,uint32_t> gpu,gmu;int accesses=0,fail=-1;};
static int access(Io *s) {return s->accesses++==s->fail;}
static int rd(void *p,uint32_t r,uint32_t *v) {auto *s=static_cast<Io *>(p);if(access(s))return 1;*v=s->gpu[r];return 0;}
static int wr(void *p,uint32_t r,uint32_t v) {auto *s=static_cast<Io *>(p);if(access(s))return 1;s->gpu[r]=v;return 0;}
static int gm(void *p,uint32_t r,uint32_t v) {auto *s=static_cast<Io *>(p);if(access(s))return 1;s->gmu[r]=v;return 0;}
static int barrier(void *p) {return access(static_cast<Io *>(p));}
int main() {
    Io s;K70GpuHwIo io{{&s,wr,rd,barrier},gm};
    CHECK(k70_gpu_hw_init(&io,0x100000)==kIOReturnSuccess);
    CHECK(s.gpu[A740_CP_SQE_INSTR_BASE]==0x100000 && s.gpu[A740_CP_SQE_INSTR_BASE+1]==0);
    CHECK(s.gpu[A740_UCHE_GMEM_RANGE_MIN]==0x1000000 && s.gpu[A740_UCHE_GMEM_RANGE_MAX]==0x12fffff);
    CHECK(s.gpu[A740_CP_PROTECT_CNTL]==0xb && s.gpu[A740_CP_APERTURE_CNTL_HOST]==0);
    CHECK(s.gpu[A740_RBBM_INT_0_MASK]==0 && s.gmu[A740_GPU_GMU_AO_GMU_CGC_MODE_CNTL]==0x20202);
    const int total=s.accesses;
    for(int n=0;n<total;++n) {Io broken;broken.fail=n;io.gpu.cookie=&broken;CHECK(k70_gpu_hw_init(&io,0x100000)==kIOReturnIOError);CHECK(broken.accesses==n+1);}
    Io bad;io.gpu.cookie=&bad;CHECK(k70_gpu_hw_init(&io,3)==kIOReturnBadArgument && !bad.accesses);
    std::printf("PASS: A740 hardware sequence and immediate refusal at all %d I/O failure points; no real GPU execution\n",total);
}
