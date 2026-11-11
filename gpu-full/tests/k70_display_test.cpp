#include <DisplayTestKernel.h>
#include <string>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(1);}} while(0)
extern "C" {int vermeer_console_muted=1;}
extern "C" void proc_selfname(char *out,int n){std::snprintf(out,(size_t)n,"%s",test_allowed?"backboardd":"other");}
extern "C" void vermeer_debug_frame_register(void (*)(void)){}
/* Compile the production implementations, including the SwapEnd caller. */
#include "../K70Display/K70FrameHandoff.cpp"
#include "../K70Display/SweetFramebuffer.cpp"
static std::vector<SweetSurface *> surfaces;
void SweetSurface::free(){if(memory)memory->release();}
uintptr_t SweetSurface::kernelAddress(){return memory?(uintptr_t)memory->getBytesNoCopy():0;}
SweetSurface *sweet_surface_copy(uint32_t id){for(auto *s:surfaces)if(s->id==id){s->retain();return s;}return nullptr;}
SweetSurface *sweet_surface_create_buffer(uint32_t w,uint32_t h,uint32_t stride,uint32_t format) {
    auto *s=new SweetSurface{};s->id=(uint32_t)surfaces.size()+1;s->width=w;s->height=h;
    s->bytesPerRow=stride;s->pixelFormat=format;s->bytesPerElement=4;s->elementWidth=s->elementHeight=1;
    s->allocSize=(stride*h+PAGE_SIZE-1)&~(PAGE_SIZE-1);s->memory=new IOMemoryDescriptor(s->allocSize);
    surfaces.push_back(s);return s;
}
SweetSurface *sweet_surface_create_physical(uint64_t,uint32_t w,uint32_t h,uint32_t stride,uint32_t format){return sweet_surface_create_buffer(w,h,stride,format);}
mach_port_name_t sweet_surface_port_in_task(SweetSurface *s,task_t){return s->id;}
static task_t owner=(void *)2;
static IOReturn armOwner(){const NGPArm a{NGP_VERSION,0};return native_gop_handoff_call(NGP_METHOD_ARM,owner,&a,sizeof a,nullptr,0);}
static IOReturn waitFrame(NGPScaleSwap &s){const NGPWait w{1};return native_gop_handoff_call(NGP_METHOD_WAIT_SWAP,owner,&w,sizeof w,&s,sizeof s);}
static IOReturn complete(uint64_t seq,unsigned status,unsigned flags){const NGPComplete c{seq,status,flags};return native_gop_handoff_call(NGP_METHOD_COMPLETE_SWAP,owner,&c,sizeof c,nullptr,0);}
static IOMemoryDescriptor *registerPanel() {
    auto *panel=new IOMemoryDescriptor(5760*3200);
    CHECK(k70_frame_register(panel,0xb8000000,1440,3200,5760)==0);
    CHECK(panel->refs()==2);return panel;
}
static K70FrameInput input(std::vector<uint8_t> &pixels,int dx=0,int dy=0,int w=1125,int h=2436) {
    K70FrameInput in{};in.base=pixels.data();in.bytes=pixels.size();in.stride=4544;
    CHECK(vframe_scale_plan(&in.scale,0,0,dx,dy,w,h,1125,2436,in.stride,0,in.bytes,
        1125,2436,1440,3200,5760,5760*3200));return in;
}
static void pattern(std::vector<uint8_t> &p,unsigned seed) {for(size_t i=0;i<p.size();++i)p[i]=(uint8_t)(seed+i*13+(i/4544)*7);}
static void checkSnapshot(const NGPScaleLayer &layer,const K70FrameInput &in,const std::vector<uint8_t> &original) {
    const auto &p=in.scale;
    CHECK(layer.lx==p.lx && layer.ly==p.ly && layer.x0==p.x0 && layer.y0==p.y0 && layer.x1==p.x1 && layer.y1==p.y1);
    const auto *read=(const uint8_t *)(uintptr_t)layer.address;
    /* Compare every scaled pixel against the immutable submitted image. */
    for(uint32_t y=p.y0;y<p.y1;++y)for(uint32_t x=p.x0;x<p.x1;++x) {
        const auto sx=vframe_scale_x(&p,x),sy=vframe_scale_y(&p,y);
        CHECK(!std::memcmp(read+size_t(sy)*layer.stride+size_t(sx)*4,
            original.data()+p.src+size_t(sy)*in.stride+size_t(sx)*4,4));
    }
}
static void snapshotAndComplete() {
    registerPanel();CHECK(armOwner()==0);CHECK(test_allocations==4 && test_task_refs==1);
    std::vector<uint8_t> pixels(4544*2436);pattern(pixels,17);const auto original=pixels;
    K70FrameInput frames[4]={input(pixels),input(pixels,-9,13,1100,2300),input(pixels,71,83,97,113),input(pixels,1124,2435,1,1)};
    CHECK(k70_frame_offer(frames,4)==K70_FRAME_GPU);
    std::memset(pixels.data(),0xee,pixels.size());
    NGPScaleSwap swap{};CHECK(waitFrame(swap)==0 && swap.count==4);
    CHECK(test_live_maps==4 && test_allocations==4);
    for(unsigned i=0;i<4;++i){CHECK(g.views[i]->options&kIOMapReadOnly);checkSnapshot(swap.layers[i],frames[i],original);}
    CHECK(k70_frame_offer(frames,4)==K70_FRAME_BLOCKED && !k70_frame_cpu_begin());
    checkSnapshot(swap.layers[0],frames[0],original);
    CHECK(complete(swap.sequence,0,NGP_QUIESCENT)==0 && test_live_maps==0);
    CHECK(k70_frame_offer(frames,4)==K70_FRAME_GPU);
    CHECK(waitFrame(swap)==0);checkSnapshot(swap.layers[0],frames[0],pixels);
    CHECK(test_allocations==4);CHECK(complete(swap.sequence,0,NGP_QUIESCENT)==0);
}
static void uncertain(unsigned status) {
    auto *panel=registerPanel();CHECK(armOwner()==0);
    std::vector<uint8_t> pixels(4544*2436,0x6a);auto f=input(pixels);
    CHECK(k70_frame_offer(&f,1)==K70_FRAME_GPU);NGPScaleSwap swap{};CHECK(waitFrame(swap)==0);
    const auto *snapshot=(const uint8_t *)(uintptr_t)swap.layers[0].address;
    auto *output=(uint8_t *)panel->getBytesNoCopy();output[0]=snapshot[0];
    CHECK(complete(swap.sequence,status,0)==0);
    CHECK(g.gate.state==K70_FRAME_QUARANTINED && test_live_maps==1 && test_map_releases==0);
    pixels.clear();pixels.shrink_to_fit();panel->release(); // only handoff still owns the panel
    for(unsigned n=0;n<64;++n){CHECK(!k70_frame_cpu_begin());CHECK(k70_frame_offer(nullptr,0)==K70_FRAME_BLOCKED);}
    // A late device read/write is still legal; no descriptor/map was released.
    output[5760*3200-1]=snapshot[swap.layers[0].source_bytes-1];
    CHECK(output[0]==0x6a && output[5760*3200-1]==0x6a && test_live_descriptors==5);
    CHECK(armOwner()!=0 && complete(swap.sequence,0,NGP_QUIESCENT)!=0);
    CHECK(waitFrame(swap)!=0 && test_live_maps==1 && test_allocations==4);
}
static void completionValidation(bool precommit) {
    registerPanel();CHECK(armOwner()==0);std::vector<uint8_t> pixels(4544*2436,0x37);auto f=input(pixels);
    CHECK(k70_frame_offer(&f,1)==K70_FRAME_GPU);NGPScaleSwap swap{};CHECK(waitFrame(swap)==0);
    CHECK(complete(0,0,NGP_QUIESCENT)!=0 && complete(swap.sequence+1,0,NGP_QUIESCENT)!=0);
    CHECK(complete(swap.sequence,0,2)!=0 && test_live_maps==1 && !k70_frame_cpu_begin());
    CHECK(complete(swap.sequence,precommit?1:0,NGP_QUIESCENT)==0 && test_live_maps==0);
    CHECK(complete(swap.sequence,0,NGP_QUIESCENT)!=0);
    CHECK(k70_frame_cpu_begin()==precommit);if(precommit)k70_frame_cpu_end();
}
static void mapFailure(bool malformed) {
    registerPanel();CHECK(armOwner()==0);std::vector<uint8_t> pixels(4544*2436,0x33);
    K70FrameInput frames[2]={input(pixels),input(pixels,11,13,101,97)};
    CHECK(k70_frame_offer(frames,2)==K70_FRAME_GPU);
    if(malformed)test_bad_map=true;else test_map_fail_after=1;
    NGPScaleSwap swap{};CHECK(waitFrame(swap)==kIOReturnNoMemory && test_live_maps==0);
    CHECK(k70_frame_cpu_begin());k70_frame_cpu_end();CHECK(g.gate.state==K70_FRAME_IDLE);
}
static void allocationFailure() {
    registerPanel();test_allocation_fail_after=2;CHECK(armOwner()==kIOReturnNoMemory);
    CHECK(test_allocations==2 && test_task_refs==0 && test_live_descriptors==3);
    CHECK(k70_frame_cpu_begin());k70_frame_cpu_end();
    test_allocation_fail_after=-1;CHECK(armOwner()==0 && test_allocations==4 && test_task_refs==1);
}
static void invalidAndReplacement() {
    registerPanel();CHECK(armOwner()==0);std::vector<uint8_t> pixels(4544*2436,0x33);
    auto f=input(pixels);CHECK(k70_frame_offer(&f,1)==K70_FRAME_GPU);
    pixels[0]=0x55;K70FrameInput bad[2]={f,f};bad[1].bytes=1;
    CHECK(k70_frame_offer(bad,2)==K70_FRAME_BLOCKED);CHECK(((uint8_t *)g.snapshot[0]->getBytesNoCopy())[0]==0x33);
    CHECK(k70_frame_offer(&f,1)==K70_FRAME_GPU);NGPScaleSwap swap{};CHECK(waitFrame(swap)==0 && swap.sequence==2);
    CHECK(((uint8_t *)(uintptr_t)swap.layers[0].address)[0]==0x55 && test_allocations==4);
    CHECK(complete(swap.sequence,0,NGP_QUIESCENT)==0);
    CHECK(waitFrame(swap)==kIOReturnTimeout && test_live_maps==0 && !k70_frame_cpu_begin());
}
static void cpuArmRace() {
    registerPanel();CHECK(k70_frame_cpu_begin());const unsigned attempts=test_lock_attempts.load();
    std::atomic<bool> done{false};int status=-1;
    std::thread thread([&]{status=armOwner();done=true;});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(test_lock_attempts.load()==attempts && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    CHECK(test_lock_attempts.load()>attempts && !done.load());
    k70_frame_cpu_end();thread.join();CHECK(status==0 && done.load() && !k70_frame_cpu_begin());
}
static void sequenceExhaustion() {
    registerPanel();CHECK(armOwner()==0);g.gate.sequence=UINT64_MAX-1;
    std::vector<uint8_t> pixels(4544*2436,0x29);auto f=input(pixels);
    CHECK(k70_frame_offer(&f,1)==K70_FRAME_GPU);pixels[0]=0x88;
    CHECK(k70_frame_offer(&f,1)==K70_FRAME_BLOCKED);
    NGPScaleSwap swap{};CHECK(waitFrame(swap)==0 && swap.sequence==UINT64_MAX);
    CHECK(((uint8_t *)(uintptr_t)swap.layers[0].address)[0]==0x29);
    CHECK(complete(swap.sequence,0,NGP_QUIESCENT)==0);
}
static void put32(uint8_t *rec,size_t offset,uint32_t value){std::memcpy(rec+offset,&value,4);}
static void swapEndCapture() {
    auto *fb=new AppleCLCD{};CHECK(fb->start(nullptr));CHECK(armOwner()==0);
    auto *source=(uint8_t *)fb->fCanvas->memory->getBytesNoCopy();std::memset(source,0x73,fb->fCanvas->allocSize);
    uint8_t rec[SWEET_FB_SWAP_REC_SIZE]{};put32(rec,0x54,fb->fCanvas->id);put32(rec,0x104,1);
    put32(rec,0xc4+8,1125);put32(rec,0xc4+12,2436);
    fb->swap(rec); // Actual SwapEnd entry must capture before returning to producer.
    CHECK(!fb->fDirty && g.gate.state==K70_FRAME_QUEUED && fb->fCanvas->refs()==1);
    std::memset(source,0xbc,fb->fCanvas->allocSize);
    NGPScaleSwap swap{};CHECK(waitFrame(swap)==0);
    const auto *snapshot=(const uint8_t *)(uintptr_t)swap.layers[0].address;
    CHECK(snapshot[0]==0x73 && snapshot[swap.layers[0].source_bytes-1]==0x73);
    auto *panel=(uint8_t *)fb->fSurface->memory->getBytesNoCopy();std::memset(panel,0x4d,5760*3200);
    vframe_refs refs{};refs.slot[0]=fb->fCanvas;
    fb->present(rec,&refs); // Stale deferred CPU frame, including its first clear.
    CHECK(panel[0]==0x4d && panel[5760*3200-1]==0x4d && !fb->fConsolePaused);
    CHECK(complete(swap.sequence,1,0)==0);fb->swap(rec);fb->present(rec,&refs);
    CHECK(!fb->fDirty && snapshot[0]==0x73 && panel[0]==0x4d && !fb->fConsolePaused);
}
int main(int argc,char **argv) {
    CHECK(argc==2);const std::string name=argv[1];
    if(name=="snapshot")snapshotAndComplete();
    else if(name=="timeout")uncertain(kIOReturnTimeout);
    else if(name=="unconfirmed_success")uncertain(0);
    else if(name=="completion")completionValidation(false);
    else if(name=="precommit")completionValidation(true);
    else if(name=="map_failure")mapFailure(false);
    else if(name=="bad_map")mapFailure(true);
    else if(name=="allocation_failure")allocationFailure();
    else if(name=="replace")invalidAndReplacement();
    else if(name=="cpu_arm_race")cpuArmRace();
    else if(name=="sequence_exhaustion")sequenceExhaustion();
    else if(name=="swap_end")swapEndCapture();
    else CHECK(false);
    std::printf("PASS %s (%u-byte pages)\n",argv[1],unsigned(PAGE_SIZE));
}
