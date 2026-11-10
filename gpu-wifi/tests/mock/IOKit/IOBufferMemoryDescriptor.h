#pragma once
#include <IOKit/IOTypes.h>
#include <kern/task.h>
#include <new>
#include <cstring>
extern size_t test_page;
extern int live_descriptors,prepare_status,complete_status;
extern bool scattered,short_segment,wrong_alignment,wrong_length;
extern uint64_t next_physical;
extern int readonly_prepare;
class IOMemoryDescriptor {
public:
    size_t length,alignment;void *mapping;uint64_t base;bool contiguous,read_only,prepared=false;
    IOMemoryDescriptor(size_t n,bool contig,bool ro=false):length(n),alignment(test_page),contiguous(contig),read_only(ro) {
        mapping=::operator new(n,std::align_val_t(alignment));std::memset(mapping,0xa5,n);
        base=next_physical;next_physical+=4*(n+test_page);++live_descriptors;
    }
    virtual ~IOMemoryDescriptor() {::operator delete(mapping,std::align_val_t(alignment));--live_descriptors;}
    int prepare() {if(prepare_status)return prepare_status;if(readonly_prepare && !read_only)return 2;prepared=true;return 0;}
    int complete() {if(complete_status)return complete_status;prepared=false;return 0;}
    void release() {if(prepared)__builtin_trap();delete this;}
    size_t getLength() {return length+(wrong_length?alignment:0);}
    void *getBytesNoCopy() {return static_cast<unsigned char *>(mapping)+(wrong_alignment?4096:0);}
    uint64_t getPhysicalSegment(size_t off,IOByteCount *len,int) {
        *len=short_segment?4096:length-off;
        const uint64_t physical=base+(contiguous?off:(off/test_page)*test_page*2);
        return physical+(wrong_alignment?4096:0);
    }
    static IOMemoryDescriptor *withAddressRange(uint64_t,size_t n,IOOptionBits options,task_t) {
        return new IOMemoryDescriptor(n,!scattered,(options&3)==kIODirectionOut);
    }
};
class IOBufferMemoryDescriptor:public IOMemoryDescriptor {
public:
    IOBufferMemoryDescriptor(size_t n,bool contiguous):IOMemoryDescriptor(n,contiguous) {}
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t,IOOptionBits opts,size_t n,uint64_t mask) {
        if((mask&(test_page-1)) || (n&(test_page-1)))__builtin_trap();
        return new IOBufferMemoryDescriptor(n,(opts&kIOMemoryPhysicallyContiguous) || !scattered);
    }
};
