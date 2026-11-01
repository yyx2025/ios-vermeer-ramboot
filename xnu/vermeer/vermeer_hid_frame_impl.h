/* Included by SweetHID.cpp and by the host allocation-failure harness.
 * All physical reports come from the single touch worker. A failed allocation
 * leaves both previous states intact, so the worker can retry the whole frame. */
void
SweetHIDTouch::touch(int finger, bool down, int x, int y)
{
    if(finger<0 || finger>=VT_CONTACTS)return;
    x=x<0?0:(x>1439?1439:x);y=y<0?0:(y>3199?3199:y);
    IOLockLock(fLock);
    struct vt_frame next=fPhysical;
    IOLockUnlock(fLock);
    next.point[finger].active=down?1:0;
    next.point[finger].x=(uint16_t)x;next.point[finger].y=(uint16_t)y;
    (void)frame(&next);
}
int
SweetHIDTouch::frame(const struct vt_frame *next)
{
    if(!next)return 0;
    vermeer_debug_input_record(0);
    IOLockLock(fLock);
    struct vth_plan plan;
    int changed=vth_prepare(&fFrameState,next,&plan);
    if(changed<=0) {
        IOLockUnlock(fLock);
        return changed==0;
    }
    uint64_t now=mach_absolute_time();
    IOHIDEvent *collection=IOHIDEvent::digitizerEvent(now,0,kIOHIDDigitizerTransducerTypeHand,false,0,0,0);
    if(!collection){IOLockUnlock(fLock);return 0;}
    collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection,TRUE);
    UInt32 combined=0;unsigned count=0;
    for(unsigned i=0;i<VT_CONTACTS;++i) {
        if(!plan.include[i])continue;
        const struct vth_contact *p=&plan.next.point[i];
        IOHIDEvent *child=IOHIDEvent::digitizerEvent(now,i+2,kIOHIDDigitizerTransducerTypeFinger,
            p->down!=0,p->down?1:0,p->x,p->y);
        if(!child){collection->release();IOLockUnlock(fLock);return 0;}
        UInt32 mask=0;
        if(plan.mask[i]&VTH_TOUCH)mask|=kIOHIDDigitizerEventTouch|kIOHIDDigitizerEventRange;
        if(plan.mask[i]&VTH_POSITION)mask|=kIOHIDDigitizerEventPosition;
        if(plan.mask[i]&VTH_IDENTITY)mask|=kIOHIDDigitizerEventIdentity;
        if(plan.mask[i]&VTH_EDGE)mask|=kIOHIDDigitizerEventFromEdgeTip;
        child->setIntegerValue(kIOHIDEventFieldDigitizerIndex,i);
        child->setIntegerValue(kIOHIDEventFieldDigitizerIdentity,i+2);
        child->setIntegerValue(kIOHIDEventFieldDigitizerTouch,p->down);
        child->setIntegerValue(kIOHIDEventFieldDigitizerEventMask,mask);
        collection->appendChild(child);child->release();
        /* appendChild has a void API and can itself fail allocation. */
        if(!collection->getChildren() || collection->getChildren()->getCount()!=++count) {
            collection->release();IOLockUnlock(fLock);return 0;
        }
        combined|=mask;
    }
    collection->setFixedValue(kIOHIDEventFieldDigitizerX,plan.x);
    collection->setFixedValue(kIOHIDEventFieldDigitizerY,plan.y);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerRange,plan.any);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch,plan.any);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask,combined);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask,plan.any?1:0);
    fFrameState=plan.next;fPhysical=*next;
    IOLockUnlock(fLock);
    dispatchEvent(collection);vermeer_debug_input_record(1);collection->release();
    return 2; /* 0 retry/error, 1 unchanged, 2 complete collection dispatched */
}
