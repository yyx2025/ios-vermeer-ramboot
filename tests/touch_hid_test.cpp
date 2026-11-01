/* Exercise the actual included HID implementation with failing allocators.
 * The mock replaces IOKit storage, not the frame/dispatch implementation. */
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include "../xnu/vermeer/vermeer_touch_frame.h"
using UInt32=uint32_t;
enum { kIOHIDDigitizerTransducerTypeHand,kIOHIDDigitizerTransducerTypeFinger };
enum { kIOHIDEventFieldDigitizerCollection,kIOHIDEventFieldDigitizerIndex,
    kIOHIDEventFieldDigitizerIdentity,kIOHIDEventFieldDigitizerTouch,
    kIOHIDEventFieldDigitizerEventMask,kIOHIDEventFieldDigitizerX,
    kIOHIDEventFieldDigitizerY,kIOHIDEventFieldDigitizerRange,kIOHIDEventFieldDigitizerButtonMask };
enum { kIOHIDDigitizerEventTouch=1,kIOHIDDigitizerEventRange=2,
    kIOHIDDigitizerEventPosition=4,kIOHIDDigitizerEventIdentity=8,kIOHIDDigitizerEventFromEdgeTip=1<<11 };
#define TRUE 1
static unsigned live,attempts,fail_at,appends,fail_append,dispatched,calls;
static int lock_value;
static void IOLockLock(int *p){assert(!*p);*p=1;}
static void IOLockUnlock(int *p){assert(*p);*p=0;}
static void vermeer_debug_input_record(int x){if(!x)++calls;}
static uint64_t mach_absolute_time(){return 123;}
class IOHIDEvent;
class OSArray {
public:
    std::vector<IOHIDEvent *> value;
    unsigned getCount(){return static_cast<unsigned>(value.size());}
};
class IOHIDEvent {
public:
    unsigned refs=1;int field[9]={0};OSArray children;
    IOHIDEvent(){++live;}
    static IOHIDEvent *digitizerEvent(uint64_t,UInt32,int,bool,int,int32_t,int32_t) {
        if(++attempts==fail_at)return nullptr;
        return new IOHIDEvent;
    }
    void setIntegerValue(int k,unsigned v){field[k]=static_cast<int>(v);}
    void setFixedValue(int k,int32_t v){field[k]=v;}
    void appendChild(IOHIDEvent *c){if(++appends==fail_append)return;++c->refs;children.value.push_back(c);}
    OSArray *getChildren(){return children.value.empty()?nullptr:&children;}
    void release(){if(!--refs){for(auto c:children.value)c->release();--live;delete this;}}
};
class SweetHIDTouch {
public:
    int *fLock=&lock_value;
    struct vt_frame fPhysical={};struct vth_state fFrameState={};
    unsigned last_count=0,last_mask=0,last_down=0;
    void touch(int,bool,int,int);
    int frame(const struct vt_frame *);
    void dispatchEvent(IOHIDEvent *event) {
        assert(!lock_value);++dispatched;
        last_count=event->getChildren()->getCount();
        last_mask=static_cast<unsigned>(event->field[kIOHIDEventFieldDigitizerEventMask]);
        last_down=static_cast<unsigned>(event->field[kIOHIDEventFieldDigitizerTouch]);
    }
};
#include "../xnu/vermeer/vermeer_hid_frame_impl.h"
static void reset_failures(){attempts=appends=fail_at=fail_append=0;}
int main()
{
    SweetHIDTouch hid;struct vt_frame frame={};
    frame.point[0]={720,3100,1};frame.point[3]={720,1600,1};
    for(unsigned failure=1;failure<=3;++failure) {
        reset_failures();fail_at=failure;
        assert(hid.frame(&frame)==0 && !dispatched && !live && !lock_value);
        assert(!hid.fFrameState.point[0].down && !hid.fFrameState.point[3].down);
    }
    for(unsigned failure=1;failure<=2;++failure) {
        reset_failures();fail_append=failure;
        assert(hid.frame(&frame)==0 && !dispatched && !live && !lock_value);
    }
    reset_failures();assert(hid.frame(&frame)==2 && dispatched==1 && !live);
    assert(hid.last_count==2 && hid.last_down && (hid.last_mask&kIOHIDDigitizerEventFromEdgeTip));
    assert(hid.frame(&frame)==1 && dispatched==1 && !live);
    frame.point[0].y=2000;frame.point[3].active=0;
    reset_failures();fail_at=3;
    assert(hid.frame(&frame)==0 && dispatched==1 && hid.fFrameState.point[3].down && !live);
    reset_failures();assert(hid.frame(&frame)==2 && dispatched==2 && !live && hid.last_count==2);
    assert(hid.fFrameState.point[0].edge && !hid.fFrameState.point[3].down);
    frame.point[0].active=0;
    assert(hid.frame(&frame)==2 && dispatched==3 && !hid.last_down && !live);
    assert(hid.frame(&frame)==1 && dispatched==3 && !live);
    auto previous=hid.fFrameState;frame.point[2]={1440,1,1};
    assert(hid.frame(&frame)==0 && !std::memcmp(&previous,&hid.fFrameState,sizeof(previous)) && !live);
    assert(!hid.frame(nullptr));
    assert(calls>0);
    std::puts("Actual HID frame implementation: atomic dispatch, all allocation failures, edge and release retries PASS");
}
