#include "drivers/NativeMetal/K70MappedDisplay.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#ifdef _WIN32
#include <malloc.h>
#endif
static void *allocate(size_t page,size_t bytes) {
#ifdef _WIN32
    return _aligned_malloc(bytes,page);
#else
    void *p=nullptr;return posix_memalign(&p,page,bytes)?nullptr:p;
#endif
}
static void release(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}
int main(int argc,char **argv) {
    assert(argc==2);const size_t page=strtoull(argv[1],nullptr,10);
    const size_t sourceBytes=1125*2436*4,alphaBytes=1125*2436;
    const size_t allocation=(sourceBytes+alphaBytes+page-1)&~(page-1);
    const size_t fbBytes=(5760*3200+page-1)&~(page-1);
    auto *source=static_cast<uint8_t *>(allocate(page,allocation));
    auto *fb=static_cast<uint32_t *>(allocate(page,fbBytes));
    assert(source && fb);std::memset(source,0,allocation);
    std::fill(fb,fb+fbBytes/4,0xff123456u);
    NGPScaleSwap swap{};swap.sequence=1;swap.version=3;swap.count=1;
    auto &f=swap.layers[0];f.address=uintptr_t(source);f.allocation_bytes=allocation;
    f.source_bytes=sourceBytes;f.stride=4500;f.format=NGP_SOURCE_BGRA;
    f.lw=1125;f.lh=2436;f.pw=1440;f.ph=3200;f.x1=1440;f.y1=3200;
    k70mapped::Driver driver({fb,fbBytes,5760,1440,3200},page);
    // The legacy kernel first-present clear occurs after the baseline capture.
    std::fill(fb,fb+fbBytes/4,0u);
    assert(driver.ready() && driver.present(swap)==k70mapped::kEmpty);
    assert(fb[0]==0xff123456u && fb[1440*3200-1]==0xff123456u);
    // A lone nonzero pixel outside all previous 64 sample positions is real content.
    source[4*113+1]=0x42;assert(!k70mapped::empty(swap));
    std::memset(source,0,allocation);
    // An independent opaque alpha plane must not be mistaken for an empty frame.
    f.format=NGP_SOURCE_B3A8;f.alpha_offset=sourceBytes;f.alpha_bytes=alphaBytes;f.alpha_stride=1125;
    std::memset(source+sourceBytes,255,alphaBytes);
    assert(driver.present(swap)==k70mapped::kPresented);
    assert(fb[0]==0xff000000u && fb[1440*3200-1]==0xff000000u);
    // Opaque white XR10: independent expected bytes, including last pixel.
    const uint32_t white=894u|(894u<<10)|(894u<<20);
    std::fill(reinterpret_cast<uint32_t *>(source),reinterpret_cast<uint32_t *>(source)+sourceBytes/4,white);
    assert(driver.present(swap)==k70mapped::kPresented);
    assert(fb[0]==0xffffffffu && fb[1440*3200-1]==0xffffffffu);
    // BGRA partial rectangle: exact geometry and untouched exterior.
    f.format=NGP_SOURCE_BGRA;f.alpha_offset=f.alpha_bytes=f.alpha_stride=0;
    std::fill(reinterpret_cast<uint32_t *>(source),reinterpret_cast<uint32_t *>(source)+sourceBytes/4,0xffabcdefu);
    f.x0=100;f.y0=100;f.x1=200;f.y1=200;
    assert(driver.present(swap)==k70mapped::kPresented);
    assert(fb[100*1440+100]==0xffabcdefu && fb[199*1440+199]==0xffabcdefu);
    assert(fb[99*1440+100]==0xffffffffu && fb[200*1440+199]==0xffffffffu);
    // Malformed memory/geometry never modifies a previously valid display.
    const uint32_t prior=fb[100*1440+100];
    f.source_bytes=4;assert(driver.present(swap)==k70mapped::kInvalid);
    assert(fb[100*1440+100]==prior);f.source_bytes=sourceBytes;
    f.address++;assert(driver.present(swap)==k70mapped::kInvalid);f.address--;
    swap.count=5;assert(driver.present(swap)==k70mapped::kInvalid);swap.count=1;
    f.x0=200;assert(driver.present(swap)==k70mapped::kInvalid);
    release(source);release(fb);
    std::printf("PASS mapped display pages=%zu: empty preservation, sparse content, independent alpha, opaque black, XR10 white, clipped publish, malformed refusal\n",page);
}
