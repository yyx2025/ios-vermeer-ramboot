/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* The pool (see reservePool): a free list of page extents over one contiguous block; best fit, neighbours merged on release. */
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The pool's write-combine (writeCombinePool): bit 31 once tried, the low bits its status. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}   /* [0] pool MiB, [1] BOs from the pool, [2] fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans */
namespace {
struct PoolExtent {size_t offset,bytes;};
struct Pool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0,peak=0;
    PoolExtent free[512];unsigned count=0;
    bool write_combined=false,quarantined=false;size_t cpu_page_bytes=0;
} pool;
bool poolTake(size_t bytes,size_t *offset) {
    int best=-1;
    for(unsigned i=0;i<pool.count;++i)if(pool.free[i].bytes>=bytes && (best<0 || pool.free[i].bytes<pool.free[best].bytes))best=(int)i;
    if(best<0)return false;
    *offset=pool.free[best].offset;
    pool.free[best].offset+=bytes;pool.free[best].bytes-=bytes;
    if(!pool.free[best].bytes) {for(unsigned i=(unsigned)best;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];--pool.count;}
    pool.used+=bytes;if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    return true;
}
void poolGive(size_t offset,size_t bytes) {
    unsigned at=0;while(at<pool.count && pool.free[at].offset<offset)++at;
    if(pool.count>=512) {pool.used-=bytes;return;}   /* cannot happen with bounded fragmentation; the extent is lost, not reused */
    for(unsigned i=pool.count;i>at;--i)pool.free[i]=pool.free[i-1];
    pool.free[at]={offset,bytes};++pool.count;
    if(at+1<pool.count && pool.free[at].offset+pool.free[at].bytes==pool.free[at+1].offset) {
        pool.free[at].bytes+=pool.free[at+1].bytes;
        for(unsigned i=at+1;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    if(at && pool.free[at-1].offset+pool.free[at-1].bytes==pool.free[at].offset) {
        pool.free[at-1].bytes+=pool.free[at].bytes;
        for(unsigned i=at;i+1<pool.count;++i)pool.free[i]=pool.free[i+1];
        --pool.count;
    }
    pool.used-=bytes;native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.quarantined?0:pool.bytes;}
/* int-152: a user BO's pages changed attribute one pmap_set_cache_attributes call at a time (each walks the page's mappings, flushes the
 * TLB and cleans the page), on allocation and again on release, under the connection lock: 3 ms a BO on average and 90-130 ms for a
 * surface-sized one, while every other GPU call of every client waited. The pool is the BOs' own block and nothing else maps it, so
 * its pages take the attribute once here and keep it while the pool lives. */
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    if(!pool.descriptor || !pool.virt || !pool.bytes || pool.quarantined)return kIOReturnNotReady;
    if(pool.write_combined)return kIOReturnSuccess;
    IOReturn status=NativeAdrenoMaintainOwnedPages(pool.virt,pool.bytes,NativeAdrenoCleanInvalidate);
    if(status)return status;
    const NativeAdrenoPhysicalSpan span={0,pool.physical,pool.bytes};
    status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    pool.write_combined=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!bytes || !outputWidth(bits))return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpuNative(&cpu);if(status)return status;
    const size_t page=(size_t)cpu.kernel_page_bytes;
    if(bytes&(page-1))return kIOReturnBadArgument;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~(uint64_t)(page-1));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status) {descriptor->release();return status;}
    IOByteCount length=0;
    const uint64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    bool valid=first && !(first&(page-1)) && virt && !((uintptr_t)virt&(page-1)) &&
        bytes<=UINTPTR_MAX-(uintptr_t)virt && descriptor->getLength()==bytes &&
        first<limit && bytes<=limit-first;
    for(size_t offset=0;valid && offset<bytes;offset+=page) {
        const uint64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        valid=physical==first+offset && length>=page;
    }
    if(!valid) {
        const IOReturn cleanup=descriptor->complete();
        if(cleanup) {pool.descriptor=descriptor;pool.quarantined=true;return cleanup;}
        descriptor->release();return kIOReturnUnsupported;
    }
    pool.descriptor=descriptor;pool.virt=virt;pool.physical=first;pool.bytes=bytes;pool.cpu_page_bytes=page;
    pool.used=pool.peak=0;pool.free[0]={0,bytes};pool.count=1;
    native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);
    return kIOReturnSuccess;
}
/* Build CPU-owned spans first, then clip a separate GPU view to the requested length.
 * A padded last CPU page never publishes padding into the GPU address space. */
IOReturn NativeAdrenoMemory::collectSpans(IOMemoryDescriptor *descriptor,unsigned bits,bool contiguous)
{
    const uint64_t limit=UINT64_C(1)<<bits;
    span_capacity_=owned_bytes_/cpu_page_bytes_;
    owned_spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!owned_spans_ || !spans_)return kIOReturnNoMemory;
    for(size_t offset=0;offset<owned_bytes_;offset+=cpu_page_bytes_) {
        IOByteCount length=0;
        const uint64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&(cpu_page_bytes_-1)) || length<cpu_page_bytes_ ||
            physical>=limit || cpu_page_bytes_>limit-physical ||
            (contiguous && offset && physical!=owned_spans_[0].physical+offset)) {
            native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
            return kIOReturnUnsupported;
        }
        if(owned_span_count_ && physical==owned_spans_[owned_span_count_-1].physical+owned_spans_[owned_span_count_-1].bytes)
            owned_spans_[owned_span_count_-1].bytes+=cpu_page_bytes_;
        else owned_spans_[owned_span_count_++]={offset,physical,cpu_page_bytes_};
    }
    for(size_t i=0;i<owned_span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=owned_spans_[i];
        if(span.offset>=bytes_)break;
        const uint64_t remaining=bytes_-span.offset;
        spans_[span_count_++]={span.offset,span.physical,span.bytes<remaining?span.bytes:remaining};
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::allocateCpuPages(size_t bytes,unsigned bits,bool contiguous)
{
    NativeAdrenoCpuState cpu{};size_t budget=0;
    const IOReturn status=NativeAdrenoQueryCpuNative(&cpu);if(status)return status;
    if(!NativeAdrenoOwnedBytes(bytes,(size_t)cpu.kernel_page_bytes,&budget))return kIOReturnBadArgument;
    return allocate(bytes,bits,contiguous,budget);
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled)
{
    if(backed() || spans_ || owned_spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>256u*1024u*1024u)
        return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpuNative(&cpu);if(status)return status;
    size_t owned=0;const size_t page=(size_t)cpu.kernel_page_bytes;
    if(!NativeAdrenoOwnedBytes(bytes,page,&owned))return kIOReturnBadArgument;
    if(owned>budget)return kIOReturnNoResources;
    const uint64_t limit=UINT64_C(1)<<bits;
    if(pooled && pool.descriptor && !pool.quarantined && pool.cpu_page_bytes==page && owned<=pool.bytes) {
        size_t offset=0;
        if(poolTake(owned,&offset)) {
            const uint64_t physical=pool.physical+offset;
            if(physical>=limit || owned>limit-physical) {poolGive(offset,owned);}
            else {
                span_capacity_=1;
                spans_=IONewData(NativeAdrenoPhysicalSpan,1);
                owned_spans_=IONewData(NativeAdrenoPhysicalSpan,1);
                if(!spans_ || !owned_spans_) {
                    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,1);
                    if(owned_spans_)IODeleteData(owned_spans_,NativeAdrenoPhysicalSpan,1);
                    spans_=owned_spans_=nullptr;span_capacity_=0;poolGive(offset,owned);return kIOReturnNoMemory;
                }
                span_count_=owned_span_count_=1;spans_[0]={0,physical,bytes};owned_spans_[0]={0,physical,owned};
                mapping_=pool.virt+offset;bytes_=bytes;owned_bytes_=owned;cpu_page_bytes_=page;
                pooled_=true;pool_offset_=offset;prepared_=true;
                bzero(mapping_,owned);
                if(pool.write_combined)NativeAdrenoPublishBarrier();
                else status=NativeAdrenoMaintainOwnedPages(mapping_,owned,NativeAdrenoClean);
                if(status) {(void)release();return status;}
                native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;return kIOReturnSuccess;
            }
        }
        native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    }
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,owned,(limit-1)&~(uint64_t)(page-1));
    if(!descriptor_)return kIOReturnNoMemory;
    status=descriptor_->prepare();
    if(status) {descriptor_->release();descriptor_=nullptr;return status;}
    prepared_=true;bytes_=bytes;owned_bytes_=owned;cpu_page_bytes_=page;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!mapping_ || ((uintptr_t)mapping_&(page-1)) || owned>UINTPTR_MAX-(uintptr_t)mapping_ ||
        descriptor_->getLength()!=owned)goto failed;
    status=collectSpans(descriptor_,bits,contiguous);if(status)goto failed;
    bzero(mapping_,owned);
    status=NativeAdrenoMaintainOwnedPages(mapping_,owned,NativeAdrenoClean);if(status)goto failed;
    native_adreno_pool_stats[5]=native_adreno_pool_stats[5]+1;native_adreno_pool_stats[6]=native_adreno_pool_stats[6]+(uint32_t)span_count_;
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(backed() || spans_ || owned_spans_ || quarantined_)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpuNative(&cpu);if(status)return status;
    cpu_page_bytes_=(size_t)cpu.kernel_page_bytes;
    if((address|bytes)&(cpu_page_bytes_-1))return kIOReturnUnsupported;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=owned_bytes_=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    status=collectSpans(imported_,bits,false);
    if(status) {native_adreno_import_stage=18;goto failed;}
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)span_count_;
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(!(descriptor_ || pooled_) || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    if(pooled_ && pool.write_combined) {write_combined_=true;return kIOReturnSuccess;}
    IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,owned_bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    status=NativeAdrenoSetPagesCacheMode(owned_spans_,owned_span_count_,true);
    if(status)return status;
    write_combined_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    if(write_combined_ && pooled_ && pool.write_combined)write_combined_=false;   /* the pool's pages keep the attribute */
    if(write_combined_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(owned_spans_,owned_span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(imported_) {
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    if(pooled_) {poolGive(pool_offset_,owned_bytes_);pooled_=false;prepared_=false;pool_offset_=0;}
    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    if(owned_spans_)IODeleteData(owned_spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    mapping_=nullptr;spans_=owned_spans_=nullptr;bytes_=owned_bytes_=cpu_page_bytes_=span_count_=span_capacity_=owned_span_count_=0;read_only_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(imported_)return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    if(write_combined_)   /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        return include_write_combined?maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean):kIOReturnSuccess;
    const size_t begin=offset&~(cpu_page_bytes_-1),end=(offset+bytes+cpu_page_bytes_-1)&~(cpu_page_bytes_-1);
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+begin,end-begin,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_)return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);
    if(write_combined_)return kIOReturnSuccess;
    const size_t begin=offset&~(cpu_page_bytes_-1),end=(offset+bytes+cpu_page_bytes_-1)&~(cpu_page_bytes_-1);
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+begin,end-begin,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    const size_t page_mask=cpu_page_bytes_-1;
    const size_t owned_begin=offset&~page_mask,owned_end=(offset+bytes+page_mask)&~page_mask;
    for(size_t i=0;i<owned_span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=owned_spans_[i];
        const size_t begin=owned_begin>span.offset?owned_begin:span.offset;
        const size_t end=owned_end<span.offset+span.bytes?owned_end:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
