#include <cstdio>
#include <cstdlib>
#include <vector>
#include "NativeAdrenoMemory.h"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
size_t test_page=TEST_PAGE_BYTES;
int allocation_fail_after=-1,live_descriptors=0,prepare_status=0,complete_status=0,readonly_prepare=0;
bool scattered=false,short_segment=false,wrong_alignment=false,wrong_length=false;
uint64_t next_physical=UINT64_C(0x8c0000000);
std::vector<NativeAdrenoPhysicalSpan> changed,maintained;
size_t last_maintained_bytes=0;void *last_maintained_mapping=nullptr;
bool restore_fails=false;
IOReturn NativeAdrenoQueryCpuNative(NativeAdrenoCpuState *out) {*out={};out->kernel_page_bytes=test_page;return 0;}
void NativeAdrenoPublishBarrier() {}
IOReturn NativeAdrenoMaintainOwnedPages(void *p,size_t n,NativeAdrenoCacheOperation) {
    CHECK(p && n && !(((uintptr_t)p|n)&(test_page-1)));
    last_maintained_mapping=p;last_maintained_bytes=n;return 0;
}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t p,size_t n,NativeAdrenoCacheOperation) {
    CHECK(p && n && !((p|n)&(test_page-1)));maintained.push_back({0,p,n});return 0;
}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *s,size_t count,bool wc) {
    if(!wc && restore_fails)return kIOReturnIOError;
    changed.clear();for(size_t i=0;i<count;++i) {CHECK(!((s[i].physical|s[i].bytes)&(test_page-1)));changed.push_back(s[i]);}
    return 0;
}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *s,size_t n,uint32_t *) {
    return NativeAdrenoSetPagesCacheMode(s,n,true);
}
#include "NativeAdrenoMemory.cpp"
void freeTestPool() {
    CHECK(!pool.used);if(pool.descriptor) {complete_status=0;CHECK(pool.descriptor->complete()==0);pool.descriptor->release();}
    pool=Pool{};
}
int main() {
    size_t rounded=0;
    CHECK(NativeAdrenoOwnedBytes(4096,test_page,&rounded) && rounded==test_page);
    CHECK(!NativeAdrenoOwnedBytes(SIZE_MAX&~size_t(4095),16384,&rounded));
    CHECK(NativeAdrenoCpuGeometry(UINT64_C(2)<<30,0x4000,1,4096,4096,4096));
    CHECK(NativeAdrenoCpuGeometry((UINT64_C(2)<<14)|(UINT64_C(1)<<30),0x4000,1,16384,16384,16384));
    CHECK(!NativeAdrenoCpuGeometry(UINT64_C(2)<<30,0x4000,1,16384,16384,16384));
    CHECK(!NativeAdrenoCpuGeometry(UINT64_C(2)<<30,0x4000,0,4096,4096,4096));
    CHECK(!NativeAdrenoCpuGeometry(UINT64_C(2)<<30,0x4000,1,4096,4096,16384));
    NativeAdrenoMemory one;
    if(test_page==16384)CHECK(one.allocate(4096,40,false,4096)==kIOReturnNoResources && live_descriptors==0);
    CHECK(one.allocate(4096,40,false,test_page)==0);
    CHECK(one.bytes()==4096 && one.ownedBytes()==test_page && one.descriptor()->getLength()==test_page);
    CHECK(one.spanCount()==1 && one.spans()[0].bytes==4096);
    for(size_t i=0;i<test_page;++i)CHECK(static_cast<unsigned char *>(one.mapping())[i]==0);
    CHECK(one.publishForDevice(0,4096,true)==0 && last_maintained_bytes==test_page);
    CHECK(one.acquireDeviceWrites(0,4096)==0 && last_maintained_bytes==test_page);
    CHECK(one.makeWriteCombined()==0 && changed.size()==1 && changed[0].bytes==test_page);
    uint32_t reset=0;CHECK(one.reassertWriteCombined(&reset)==0 && changed[0].bytes==test_page);
    CHECK(one.holdForDevice()==0 && one.release()==kIOReturnBusy);
    CHECK(one.retireAfterQuiescence()==0 && one.release()==0 && live_descriptors==0);
    scattered=true;
    NativeAdrenoMemory many;
    CHECK(many.allocate(test_page+4096,40,false,3*test_page)==0);
    const size_t expected_owned=test_page==4096?8192:32768;
    CHECK(many.ownedBytes()==expected_owned && many.spanCount()==2 && many.spans()[1].bytes==4096);
    CHECK(many.publishForDevice(test_page,4096,false)==0 && last_maintained_bytes==test_page);
    CHECK(last_maintained_mapping==static_cast<unsigned char *>(many.mapping())+test_page);
    CHECK(many.makeWriteCombined()==0 && changed.size()==2 && changed[1].bytes==test_page);
    CHECK(many.publishForDevice(test_page,4096,true,true)==0 && maintained.back().bytes==test_page);
    CHECK(many.release()==0 && live_descriptors==0);scattered=false;
    for(int failure=0;failure<2;++failure) {
        NativeAdrenoMemory bad;allocation_fail_after=failure;
        CHECK(bad.allocate(test_page,40,false,test_page)==kIOReturnNoMemory && live_descriptors==0);
        CHECK(!bad.backed());allocation_fail_after=-1;
    }
    if(test_page==16384) {
        NativeAdrenoMemory bad;short_segment=true;
        CHECK(bad.allocate(test_page,40,false,test_page)==kIOReturnUnsupported && live_descriptors==0);
        short_segment=false;
        CHECK(bad.importTask((void *)2,0x100000,4096,40,test_page)==kIOReturnUnsupported && live_descriptors==0);
        CHECK(bad.importTask((void *)2,0x101000,test_page,40,test_page)==kIOReturnUnsupported && live_descriptors==0);
    }
    NativeAdrenoMemory imported;readonly_prepare=1;
    CHECK(imported.importTask((void *)2,0x100000,test_page,40,test_page)==0 && imported.readOnly());
    CHECK(imported.acquireDeviceWrites(0,4096)==0 && maintained.back().bytes==test_page);
    CHECK(imported.release()==0 && live_descriptors==0);readonly_prepare=0;
    CHECK(NativeAdrenoMemory::reservePool(40,4*test_page)==0);
    NativeAdrenoMemory p,q;
    CHECK(p.allocate(4096,40,false,test_page,true)==0);
    CHECK(q.allocate(4096,40,false,test_page,true)==0);
    CHECK(q.spans()[0].physical-p.spans()[0].physical==test_page && pool.used==2*test_page);
    CHECK(p.release()==0 && q.release()==0 && pool.used==0 && pool.count==1 && pool.free[0].bytes==4*test_page);
    CHECK(NativeAdrenoMemory::writeCombinePool()==0);
    CHECK(p.allocate(4096,40,false,test_page,true)==0 && p.makeWriteCombined()==0);
    CHECK(p.release()==0 && pool.write_combined && pool.used==0);freeTestPool();
    NativeAdrenoMemory quarantined;
    CHECK(quarantined.allocate(4096,40,false,test_page)==0);complete_status=kIOReturnIOError;
    CHECK(quarantined.release()==kIOReturnIOError && quarantined.quarantined() && live_descriptors==1);
    CHECK(quarantined.allocate(4096,40,false,test_page)==kIOReturnBusy && quarantined.release()==kIOReturnBusy);
    // A separate reserve failure must also retain its wired descriptor.
    wrong_length=true;
    CHECK(NativeAdrenoMemory::reservePool(40,test_page)==kIOReturnIOError && pool.quarantined);
    CHECK(NativeAdrenoMemory::poolBytes()==0 && NativeAdrenoMemory::writeCombinePool()==kIOReturnNotReady);
    CHECK(NativeAdrenoMemory::reservePool(40,test_page)==kIOReturnBusy && live_descriptors==2);
    std::printf("Actual NativeAdrenoMemory: %zu-byte CPU pages, padding, budgets, imports, pool isolation, cache spans and quarantine: PASS\n",test_page);
}
