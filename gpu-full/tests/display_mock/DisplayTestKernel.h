#pragma once
/* Software IOKit peer for compiling the real display handoff/framebuffer.
 * Tracks ownership and injects allocation/map failures; never models GPU DMA. */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#define PAGE_SIZE TEST_PAGE_BYTES
#define APPLE_KEXT_OVERRIDE
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,B)
#define OSDynamicCast(T,p) dynamic_cast<T *>(p)
#define OSSafeReleaseNULL(p) do {if(p){(p)->release();(p)=nullptr;}} while(0)
#define _Atomic
#define os_atomic_load(p,o) __atomic_load_n((p),__ATOMIC_RELAXED)
#define os_atomic_inc(p,o) __atomic_fetch_add((p),1,__ATOMIC_RELAXED)
#define os_atomic_add(p,n,o) __atomic_fetch_add((p),(n),__ATOMIC_RELAXED)
#define bzero(p,n) std::memset(p,0,n)
using IOReturn=int;using IOOptionBits=uint32_t;using IOByteCount=size_t;
using UInt32=uint32_t;using task_t=void *;using mach_port_t=uint32_t;
using mach_port_name_t=uint32_t;using io_user_reference_t=uint64_t;using mach_vm_address_t=uint64_t;
inline task_t kernel_task=(void *)1;
#define TASK_NULL nullptr
enum {kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNoMemory=2,kIOReturnNotReady=3,kIOReturnTimeout=4};
enum {kIODirectionInOut=3,kIOMemoryKernelUserShared=4,kIOMapAnywhere=8,kIOMapReadOnly=16};
enum {THREAD_UNINT=0,THREAD_TIMED_OUT=1,kMillisecondScale=1000000,MACH_PORT_NULL=0,kIOUserNotifyOptionCanDrop=1};
inline int test_allocation_fail_after=-1,test_map_fail_after=-1,test_live_descriptors=0,test_live_maps=0;
inline int test_allocations=0,test_map_releases=0,test_task_refs=0;
inline bool test_bad_map=false,test_allowed=true,test_quiet=true;
inline std::atomic<unsigned> test_lock_attempts{0};
inline void test_require(bool ok) {if(!ok)std::abort();}
class OSObject {
    int refs_=1;
public:
    virtual ~OSObject()=default;
    virtual bool init(){return true;}
    virtual void free(){}
    void retain(){++refs_;}
    void release(){if(!--refs_){free();delete this;}}
    int refs()const{return refs_;}
};
class IOMemoryMap;
class IOMemoryDescriptor:public OSObject {
    size_t length_;
    void *data_;
public:
    explicit IOMemoryDescriptor(size_t n):length_(n),data_(::operator new(n,std::align_val_t(PAGE_SIZE))) {
        std::memset(data_,0,n);++test_live_descriptors;
    }
    ~IOMemoryDescriptor(){::operator delete(data_,std::align_val_t(PAGE_SIZE));--test_live_descriptors;}
    size_t getLength()const{return length_;}
    void *getBytesNoCopy(){return data_;}
    IOMemoryMap *createMappingInTask(task_t,uint64_t,IOOptionBits);
};
class IOMemoryMap:public OSObject {
    IOMemoryDescriptor *memory_;
public:
    IOOptionBits options;
    IOMemoryMap(IOMemoryDescriptor *m,IOOptionBits o):memory_(m),options(o){m->retain();++test_live_maps;}
    ~IOMemoryMap(){memory_->release();--test_live_maps;++test_map_releases;}
    uintptr_t getVirtualAddress(){return test_bad_map?0:(uintptr_t)memory_->getBytesNoCopy();}
    size_t getLength(){return memory_->getLength();}
};
inline IOMemoryMap *IOMemoryDescriptor::createMappingInTask(task_t,uint64_t,IOOptionBits options) {
    if(test_map_fail_after==0)return nullptr;
    if(test_map_fail_after>0)--test_map_fail_after;
    return new IOMemoryMap(this,options);
}
class IOBufferMemoryDescriptor:public IOMemoryDescriptor {
public:
    explicit IOBufferMemoryDescriptor(size_t n):IOMemoryDescriptor(n){}
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t,IOOptionBits,size_t n,size_t align) {
        test_require(align==PAGE_SIZE && !(n%PAGE_SIZE));
        if(test_allocation_fail_after==0)return nullptr;
        if(test_allocation_fail_after>0)--test_allocation_fail_after;
        ++test_allocations;return new IOBufferMemoryDescriptor(n);
    }
};
struct IOLock {std::mutex mutex;std::condition_variable condition;};
inline IOLock *IOLockAlloc(){return new IOLock;}
inline void IOLockLock(IOLock *lock){++test_lock_attempts;lock->mutex.lock();}
inline void IOLockUnlock(IOLock *lock){lock->mutex.unlock();}
inline void IOLockWakeup(IOLock *lock,void *,bool){lock->condition.notify_all();}
inline uint64_t mach_absolute_time(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void clock_interval_to_deadline(uint32_t n,int unit,uint64_t *deadline){*deadline=mach_absolute_time()+uint64_t(n)*unit;}
inline int IOLockSleepDeadline(IOLock *lock,void *,uint64_t deadline,int) {
    std::unique_lock<std::mutex> hold(lock->mutex,std::adopt_lock);
    const auto result=lock->condition.wait_until(hold,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
    hold.release();return result==std::cv_status::timeout?THREAD_TIMED_OUT:0;
}
inline void task_reference(task_t){++test_task_refs;}
inline void *IOMallocData(size_t n){return std::malloc(n);}
inline void IOFreeData(void *p,size_t){std::free(p);}
inline void IOLog(const char *,...){}
struct PE_Video {uint64_t v_baseAddr=0xb8000000;uint32_t v_width=1440,v_height=3200,v_rowBytes=5760;};
inline void PE_current_console(PE_Video *v){*v=PE_Video{};}
inline bool PE_parse_boot_argn(const char *name,void *out,size_t){if(!std::strcmp(name,"sweetfb_noconsole")){*(int *)out=test_quiet;return true;}return false;}
inline const char *kIOUserClientClassKey="IOUserClientClass";
class IOService:public OSObject {
public:
    virtual bool start(IOService *){return true;}
    template<class... T> void setProperty(T...){}
    void registerService(){}
    void terminate(){}
};
class OSArray:public OSObject {
    std::vector<OSObject *> objects_;
public:
    static OSArray *withCapacity(unsigned){return new OSArray;}
    unsigned getCount()const{return (unsigned)objects_.size();}
    OSObject *getObject(unsigned i){return objects_[i];}
    void setObject(OSObject *p){objects_.push_back(p);}
    unsigned getNextIndexOfObject(OSObject *p,unsigned){for(unsigned i=0;i<getCount();++i)if(objects_[i]==p)return i;return unsigned(-1);}
    void removeObject(unsigned i){objects_.erase(objects_.begin()+i);}
};
class IOTimerEventSource:public OSObject {
public:
    static IOTimerEventSource *timerEventSource(OSObject *,void (*)(OSObject *,IOTimerEventSource *)){return new IOTimerEventSource;}
    void setTimeoutUS(unsigned){}
};
class IOWorkLoop:public OSObject {
public:
    static IOWorkLoop *workLoop(){return new IOWorkLoop;}
    IOReturn addEventSource(IOTimerEventSource *){return 0;}
};
struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
    IOMemoryDescriptor *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    size_t structureInputSize=0,structureOutputSize=0;
    void *structureInput=nullptr,*structureOutput=nullptr;
    uint32_t scalarInputCount=0,scalarOutputCount=0;
    uint64_t *scalarInput=nullptr,*scalarOutput=nullptr;
};
using OSAsyncReference64=uint64_t[8];
class IOUserClient:public IOService {
public:
    bool initWithTask(task_t,void *,UInt32){return true;}
    void releaseNotificationPort(mach_port_t){}
    template<class... T> void setAsyncReference64(T...){}
    template<class... T> void sendAsyncResult64WithOptions(T...){}
};
