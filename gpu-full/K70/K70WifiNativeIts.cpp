/* SPDX-License-Identifier: MIT */
#include "K70WifiNativeIts.h"
#include <string.h>

namespace {
constexpr uint64_t ItsPhysical=UINT64_C(0x17140000),RdistPhysical=UINT64_C(0x17180000);
constexpr uint64_t Address48=UINT64_C(0x0000ffffffff0000);
constexpr uint64_t Valid=UINT64_C(1)<<63,InnerShareable=UINT64_C(1)<<10;
constexpr uint64_t ItsCacheMask=UINT64_C(7)<<59,ShareMask=UINT64_C(3)<<10;
constexpr uint64_t ItsWriteBack=UINT64_C(7)<<59,ItsNonCache=UINT64_C(1)<<59;
constexpr uint64_t RdistCacheMask=UINT64_C(7)<<7,RdistWriteBack=UINT64_C(7)<<7,RdistNonCache=UINT64_C(1)<<7;
constexpr uint32_t Enable=1,Quiescent=UINT32_C(1)<<31,RdistRwp=8;
constexpr uint32_t Ctlr=0,Typer=8,Waker=0x14,Cbaser=0x80,Cwriter=0x88,Creader=0x90,Baser=0x100;
constexpr uint32_t Propbaser=0x70,Pendbaser=0x78,LpiFirst=8192;
constexpr size_t TableAlignment=65536,QueueBytes=65536,CommandBytes=32;
enum { Queue=0,DeviceTable=1,CollectionTable=2,Itt=3,Properties=4,Pending=5 };
struct Command { uint64_t word[4]; };
struct PeRecord {uint64_t physical;uint32_t affinity,ready;};
PeRecord pes[8]{};
K70WifiItsRuntime *controller;
uint32_t controller_claimed;

static bool all_ops(const K70WifiItsOps *o)
{
    return o && o->claim_global && o->release_unpublished && o->allocate && o->release_memory &&
        o->hold_memory && o->quarantine_memory && o->clean && o->barrier && o->read32 && o->read64 &&
        o->write32 && o->write64 && o->now_ns && o->relax && o->map_msi_doorbell && o->wake_worker && o->deliver_irq;
}
static int read32(K70WifiItsRuntime *r,unsigned s,uint32_t off,uint32_t *v)
{ return r->ops.read32(r->ops.cookie,s,off,v); }
static int read64(K70WifiItsRuntime *r,unsigned s,uint32_t off,uint64_t *v)
{ return r->ops.read64(r->ops.cookie,s,off,v); }
static int write32(K70WifiItsRuntime *r,unsigned s,uint32_t off,uint32_t v)
{ r->ops.barrier(r->ops.cookie); return r->ops.write32(r->ops.cookie,s,off,v); }
static int write64(K70WifiItsRuntime *r,unsigned s,uint32_t off,uint64_t v)
{ r->ops.barrier(r->ops.cookie); return r->ops.write64(r->ops.cookie,s,off,v); }
static size_t rounded(size_t n) {return (n+TableAlignment-1)&~(TableAlignment-1);}
static int allocate(K70WifiItsRuntime *r,unsigned index,size_t n)
{
    if(!n || n>16*1024*1024)return K70WifiItsInvalid;
    K70WifiItsMemory &m=r->memory[index];n=rounded(n);
    int rc=r->ops.allocate(r->ops.cookie,n,TableAlignment,&m);if(rc)return rc;
    if(!m.cpu || !m.owner || !m.physical || m.bytes!=n || (m.physical&(TableAlignment-1)) ||
       (m.bytes&(TableAlignment-1)) || ((uintptr_t)m.cpu&3) || m.physical>Address48 ||
       m.bytes>UINT64_C(0x1000000000000)-m.physical || m.bytes>UINTPTR_MAX-(uintptr_t)m.cpu)
        return K70WifiItsInvalid;
    for(unsigned i=0;i<index;++i)if(r->memory[i].owner) {
        const K70WifiItsMemory &other=r->memory[i];
        const bool physical_overlap=m.physical<other.physical+other.bytes && other.physical<m.physical+m.bytes;
        const uintptr_t cpu=(uintptr_t)m.cpu,other_cpu=(uintptr_t)other.cpu;
        const bool cpu_overlap=cpu<other_cpu+other.bytes && other_cpu<cpu+m.bytes;
        if(physical_overlap || cpu_overlap)return K70WifiItsInvalid;
    }
    /* All unused IDs and padding remain zero and owned. */
    memset(m.cpu,0,m.bytes);
    return r->ops.clean(r->ops.cookie,&m);
}
static int fail(K70WifiItsRuntime *r,int rc)
{
    r->last_error=rc;
    if(r->published) {k70_wifi_its_quarantine(r,rc);return rc;}
    bool retained=false;
    for(unsigned i=0;i<K70WifiItsMemoryCount;++i)if(r->memory[i].owner) {
        const int cleanup=r->ops.release_memory(r->ops.cookie,&r->memory[i]);
        if(cleanup) {retained=true;r->ops.quarantine_memory(r->ops.cookie,&r->memory[i]);}
    }
    if(r->lease && r->ops.release_unpublished(r->ops.cookie,r->lease))retained=true;
    if(retained) {__atomic_store_n(&r->state,(uint32_t)K70WifiItsQuarantined,__ATOMIC_RELEASE);}
    else {r->lease=nullptr;__atomic_store_n(&r->state,(uint32_t)K70WifiItsReleased,__ATOMIC_RELEASE);
          __atomic_store_n(&controller_claimed,0u,__ATOMIC_RELEASE);}
    return rc;
}
/* Read-back supports a fixed shareability implementation, including an
 * explicit non-cacheable fallback. Every table publication is still cleaned
 * to PoC; no firmware coherency assumption is made. */
static int program_baser(K70WifiItsRuntime *r,unsigned space,uint32_t off,uint64_t v,bool redistributor,size_t tablebytes=0)
{
    int rc=write64(r,space,off,v);if(rc)return rc;
    uint64_t observed=0;rc=read64(r,space,off,&observed);if(rc)return rc;
    if(tablebytes && ((v^observed)&UINT64_C(0x300))) {
        const unsigned pagecode=(unsigned)((observed>>8)&3);
        if(pagecode>2)return K70WifiItsUnsupported;
        const size_t pagebytes=(size_t)4096<<(pagecode*2),pages=tablebytes/pagebytes;
        if(tablebytes%pagebytes || !pages || pages>256)return K70WifiItsUnsupported;
        v=(v&~UINT64_C(0x3ff))|(uint64_t)pagecode<<8|(pages-1);
        rc=write64(r,space,off,v);if(rc)return rc;
        rc=read64(r,space,off,&observed);if(rc)return rc;
    }
    if((v^observed)&ShareMask) {
        const uint64_t cachemask=redistributor?RdistCacheMask:ItsCacheMask;
        v=(v&~ShareMask)|(observed&ShareMask);
        if(!(observed&ShareMask))v=(v&~cachemask)|(redistributor?RdistNonCache:ItsNonCache);
        rc=write64(r,space,off,v);if(rc)return rc;
        rc=read64(r,space,off,&observed);if(rc)return rc;
    }
    /* An unsupported table geometry/address/valid field is an error. */
    return observed==v?0:K70WifiItsUnsupported;
}
static int wait_rdist(K70WifiItsRuntime *r,bool enabled)
{
    const uint64_t began=r->ops.now_ns(r->ops.cookie);
    for(unsigned bounded=0;bounded<1000000;++bounded) {
        uint32_t ctl=0;int rc=read32(r,K70WifiItsRedistributor,Ctlr,&ctl);if(rc)return rc;
        if(!(ctl&RdistRwp))return ((ctl&Enable)!=0)==enabled?0:K70WifiItsIo;
        if(r->ops.now_ns(r->ops.cookie)-began>=r->config.timeout_ns)return K70WifiItsTimeout;
        r->ops.relax(r->ops.cookie);
    }
    return K70WifiItsTimeout;
}
/* Single producer; completion waits are hardware command-queue handshakes,
 * never a replacement for delivery of device interrupts. */
static int submit(K70WifiItsRuntime *r,const Command &command,bool sync)
{
    uint64_t reader=0;int rc=read64(r,K70WifiItsMmio,Creader,&reader);if(rc)return rc;
    if(reader&1)return K70WifiItsIo; /* stalled queue; do not retry/skip */
    if(reader&~UINT64_C(0xffe1))return K70WifiItsIo;
    const uint32_t count=sync?2:1,next=(r->writer+count*CommandBytes)&(QueueBytes-1);
    /* One slot remains unused to distinguish a full queue from an empty one. */
    const uint32_t freebytes=((uint32_t)reader-r->writer-CommandBytes)&(QueueBytes-1);
    if(count*CommandBytes>freebytes)return K70WifiItsBusy;
    Command *queue=static_cast<Command *>(r->memory[Queue].cpu);
    queue[r->writer/CommandBytes]=command;
    if(sync) {Command barrier{};barrier.word[0]=0x05;barrier.word[2]=r->target;
        queue[((r->writer+CommandBytes)&(QueueBytes-1))/CommandBytes]=barrier;}
    rc=r->ops.clean(r->ops.cookie,&r->memory[Queue]);if(rc)return rc;
    rc=write64(r,K70WifiItsMmio,Cwriter,next);if(rc)return rc;
    r->writer=next;
    const uint64_t began=r->ops.now_ns(r->ops.cookie);
    for(unsigned bounded=0;bounded<1000000;++bounded) {
        rc=read64(r,K70WifiItsMmio,Creader,&reader);if(rc)return rc;
        if(reader&1 || reader&~UINT64_C(0xffe1))return K70WifiItsIo;
        if((uint32_t)reader==next) {r->ops.barrier(r->ops.cookie);return 0;}
        if(r->ops.now_ns(r->ops.cookie)-began>=r->config.timeout_ns)return K70WifiItsTimeout;
        r->ops.relax(r->ops.cookie);
    }
    return K70WifiItsTimeout;
}
static int check_cold(K70WifiItsRuntime *r,uint32_t *ctl)
{
    int rc=read32(r,K70WifiItsMmio,Ctlr,ctl);if(rc)return rc;
    if((*ctl&Enable) || !(*ctl&Quiescent))return K70WifiItsBusy;
    uint32_t rctl=0,waker=0;
    rc=read32(r,K70WifiItsRedistributor,Ctlr,&rctl);if(rc)return rc;
    rc=read32(r,K70WifiItsRedistributor,Waker,&waker);if(rc)return rc;
    if(rctl&(Enable|RdistRwp) || waker&6)return K70WifiItsBusy;
    uint64_t value=0;rc=read64(r,K70WifiItsMmio,Cbaser,&value);if(rc)return rc;
    if(value&Valid)return K70WifiItsBusy;
    for(unsigned i=0;i<8;++i) {
        rc=read64(r,K70WifiItsMmio,Baser+i*8,&value);if(rc)return rc;
        if(value&Valid)return K70WifiItsBusy;
    }
    return 0;
}
}

extern "C" int k70_wifi_its_check_config(const K70WifiItsConfig *c)
{
    if(!c || c->its_physical!=ItsPhysical || c->device_id!=0x1401 ||
       c->redistributor_physical<RdistPhysical || c->redistributor_physical>=RdistPhysical+UINT64_C(0x200000) ||
       (c->redistributor_physical-RdistPhysical)%0x40000 || c->cpu_number!=0 || c->collection_id!=0 ||
       !c->timeout_ns || c->timeout_ns>UINT64_C(1000000000))return K70WifiItsInvalid;
    return 0;
}
extern "C" void k70_wifi_its_record_pe(uint64_t physical,uint32_t cpu,uint32_t affinity)
{
    if(cpu>=8)return;
    /* Called by this PE before publication; never modifies an existing route. */
    if(__atomic_load_n(&pes[cpu].ready,__ATOMIC_ACQUIRE))return;
    pes[cpu].physical=physical;pes[cpu].affinity=affinity;
    __atomic_store_n(&pes[cpu].ready,1u,__ATOMIC_RELEASE);
}
extern "C" int k70_wifi_its_pe_route(uint32_t cpu,uint64_t *physical,uint32_t *affinity)
{
    if(cpu>=8 || !physical || !affinity)return K70WifiItsInvalid;
    if(!__atomic_load_n(&pes[cpu].ready,__ATOMIC_ACQUIRE))return K70WifiItsNotReady;
    *physical=pes[cpu].physical;*affinity=pes[cpu].affinity;return 0;
}
extern "C" void k70_wifi_its_quarantine(K70WifiItsRuntime *r,int error)
{
    if(!r)return;
    r->last_error=error;
    __atomic_store_n(&r->state,(uint32_t)K70WifiItsQuarantined,__ATOMIC_RELEASE);
    for(unsigned i=0;i<K70WifiItsMemoryCount;++i)
        if(r->memory[i].owner && r->ops.quarantine_memory)r->ops.quarantine_memory(r->ops.cookie,&r->memory[i]);
    /* Tables, lease, CPU pin and IRQ registration outlive all error states.
     * Unknown endpoint/ITS state never permits memory release. */
}
extern "C" int k70_wifi_its_init(K70WifiItsRuntime *r,const K70WifiItsConfig *c,const K70WifiItsOps *o)
{
    int rc=k70_wifi_its_check_config(c);if(rc || !r || !all_ops(o))return rc?rc:K70WifiItsInvalid;
    if(r->state!=K70WifiItsCold)return K70WifiItsBusy;
    uint64_t route=0;uint32_t affinity=0;
    if(k70_wifi_its_irq_hook_version()!=1 || k70_wifi_its_pe_route(c->cpu_number,&route,&affinity) ||
       route!=c->redistributor_physical || affinity!=c->affinity)return K70WifiItsNotReady;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&controller_claimed,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return K70WifiItsBusy;
    r->config=*c;r->ops=*o;r->state=K70WifiItsPreparing;r->vector_count=K70WifiItsVectors;
    rc=o->claim_global(o->cookie,c,&r->lease,&r->lpi_base,&r->irq_base);if(rc)return fail(r,rc);
    if(!r->lease || r->lpi_base<LpiFirst || (r->lpi_base&(K70WifiItsVectors-1)) || r->irq_base<0 ||
       r->irq_base>INT32_MAX-K70WifiItsVectors)return fail(r,K70WifiItsInvalid);
    uint32_t ctl=0;rc=check_cold(r,&ctl);if(rc)return fail(r,rc);
    rc=read64(r,K70WifiItsMmio,Typer,&r->typer);if(rc)return fail(r,rc);
    uint64_t rtyper=0;rc=read64(r,K70WifiItsRedistributor,Typer,&rtyper);if(rc)return fail(r,rc);
    if(!(r->typer&1) || !(rtyper&1) || (uint32_t)(rtyper>>32)!=c->affinity)return fail(r,K70WifiItsUnsupported);
    const unsigned devicebits=(unsigned)((r->typer>>13)&31)+1;
    const unsigned ittbytes=(unsigned)((r->typer>>4)&15)+1;
    r->id_bits=(unsigned)((r->typer>>8)&31)+1;
    if(r->id_bits>K70WifiItsMaxIdBits)r->id_bits=K70WifiItsMaxIdBits;
    if(devicebits<13 || r->id_bits<14 || r->lpi_base>(UINT32_C(1)<<r->id_bits)-K70WifiItsVectors)
        return fail(r,K70WifiItsUnsupported);
    r->target=(r->typer&(UINT64_C(1)<<19))?c->redistributor_physical:((rtyper>>8)&0xffff)<<16;
    int device_baser=-1,collection_baser=-1;uint64_t device_desc=0,collection_desc=0;
    for(unsigned i=0;i<8;++i) {uint64_t v=0;rc=read64(r,K70WifiItsMmio,Baser+i*8,&v);if(rc)return fail(r,rc);
        const unsigned type=(unsigned)((v>>56)&7);
        if(type==1) {if(device_baser>=0)return fail(r,K70WifiItsUnsupported);device_baser=(int)i;device_desc=v;}
        if(type==4) {if(collection_baser>=0)return fail(r,K70WifiItsUnsupported);collection_baser=(int)i;collection_desc=v;}}
    if(device_baser<0 || (collection_baser<0 && ((r->typer>>24)&255)==0))return fail(r,K70WifiItsUnsupported);
    const size_t device_entry=((device_desc>>48)&31)+1;
    const size_t collection_entry=((collection_desc>>48)&31)+1;
    rc=allocate(r,Queue,QueueBytes);if(rc)return fail(r,rc);
    rc=allocate(r,DeviceTable,((size_t)c->device_id+1)*device_entry);if(rc)return fail(r,rc);
    if(collection_baser>=0) {rc=allocate(r,CollectionTable,collection_entry);if(rc)return fail(r,rc);}
    rc=allocate(r,Itt,K70WifiItsVectors*ittbytes);if(rc)return fail(r,rc);
    rc=allocate(r,Properties,(UINT32_C(1)<<r->id_bits)-LpiFirst);if(rc)return fail(r,rc);
    rc=allocate(r,Pending,(UINT32_C(1)<<r->id_bits)/8);if(rc)return fail(r,rc);
    for(unsigned i=0;i<K70WifiItsMemoryCount;++i)if(r->memory[i].owner) {
        rc=o->hold_memory(o->cookie,&r->memory[i]);if(rc)return fail(r,rc);
    }
    /* hold_memory failures are still unpublished; that callback's owner must
     * undo its hold during release_memory. Once an external doorbell map or
     * BASER may have reached hardware, every resource remains pinned. */
    r->published=1;
    uint64_t doorbell=0;
    rc=o->map_msi_doorbell(o->cookie,ItsPhysical+0x10000,4096,&doorbell);if(rc)return fail(r,rc);
    if(!doorbell || (doorbell&4095) || doorbell>UINT32_MAX-4095)return fail(r,K70WifiItsInvalid);
    /* The DMA domain owns a valid page before the endpoint gets its message. */
    r->msi_iova=doorbell+0x40;
    uint64_t descriptor=r->memory[DeviceTable].physical | (device_desc&UINT64_C(0x071f000000000000)) |
        Valid | InnerShareable | ItsWriteBack | UINT64_C(0x200) | (r->memory[DeviceTable].bytes/TableAlignment-1);
    rc=program_baser(r,K70WifiItsMmio,Baser+(uint32_t)device_baser*8,descriptor,false,r->memory[DeviceTable].bytes);if(rc)return fail(r,rc);
    if(collection_baser>=0) {
        descriptor=r->memory[CollectionTable].physical|(collection_desc&UINT64_C(0x071f000000000000))|
            Valid|InnerShareable|ItsWriteBack|UINT64_C(0x200)|(r->memory[CollectionTable].bytes/TableAlignment-1);
        rc=program_baser(r,K70WifiItsMmio,Baser+(uint32_t)collection_baser*8,descriptor,false,r->memory[CollectionTable].bytes);if(rc)return fail(r,rc);
    }
    descriptor=r->memory[Queue].physical|Valid|InnerShareable|ItsWriteBack|(QueueBytes/4096-1);
    rc=program_baser(r,K70WifiItsMmio,Cbaser,descriptor,false);if(rc)return fail(r,rc);
    uint64_t reader=0;rc=read64(r,K70WifiItsMmio,Creader,&reader);if(rc || reader)return fail(r,rc?rc:K70WifiItsIo);
    rc=write64(r,K70WifiItsMmio,Cwriter,0);if(rc)return fail(r,rc);
    descriptor=r->memory[Properties].physical|InnerShareable|RdistWriteBack|(r->id_bits-1);
    rc=program_baser(r,K70WifiItsRedistributor,Propbaser,descriptor,true);if(rc)return fail(r,rc);
    descriptor=r->memory[Pending].physical|InnerShareable|RdistWriteBack;
    rc=program_baser(r,K70WifiItsRedistributor,Pendbaser,descriptor,true);if(rc)return fail(r,rc);
    rc=write32(r,K70WifiItsMmio,Ctlr,(ctl&~Quiescent)|Enable);if(rc)return fail(r,rc);
    rc=read32(r,K70WifiItsMmio,Ctlr,&ctl);if(rc || !(ctl&Enable))return fail(r,rc?rc:K70WifiItsIo);
    Command command{};command.word[0]=0x09;command.word[2]=Valid|r->target|c->collection_id;
    rc=submit(r,command,true);if(rc)return fail(r,rc);
    command={};command.word[0]=0x08|(uint64_t)c->device_id<<32;command.word[1]=4;
    command.word[2]=Valid|r->memory[Itt].physical;
    rc=submit(r,command,true);if(rc)return fail(r,rc);
    for(unsigned i=0;i<K70WifiItsVectors;++i) {
        command={};command.word[0]=0x0a|(uint64_t)c->device_id<<32;
        command.word[1]=i|(uint64_t)(r->lpi_base+i)<<32;command.word[2]=c->collection_id;
        rc=submit(r,command,true);if(rc)return fail(r,rc);
    }
    uint8_t *properties=static_cast<uint8_t *>(r->memory[Properties].cpu);
    for(unsigned i=0;i<K70WifiItsVectors;++i)properties[r->lpi_base+i-LpiFirst]=0xa3; /* priority A0, Group1, enabled */
    rc=o->clean(o->cookie,&r->memory[Properties]);if(rc)return fail(r,rc);
    command={};command.word[0]=0x0d;command.word[2]=c->collection_id;
    rc=submit(r,command,true);if(rc)return fail(r,rc);
    /* Publish the IRQ route before enabling physical LPIs. Exception dispatch
     * coalesces interrupts while the serialized wireless worker consumes rings. */
    __atomic_store_n(&controller,r,__ATOMIC_RELEASE);
    uint32_t rctl=0;rc=read32(r,K70WifiItsRedistributor,Ctlr,&rctl);if(rc)return fail(r,rc);
    rc=write32(r,K70WifiItsRedistributor,Ctlr,rctl|Enable);if(rc)return fail(r,rc);
    rc=wait_rdist(r,true);if(rc)return fail(r,rc);
    __atomic_store_n(&r->state,(uint32_t)K70WifiItsActive,__ATOMIC_RELEASE);
    return 0;
}
extern "C" int k70_wifi_its_msi_target(const K70WifiItsRuntime *r,uint32_t *lo,uint32_t *hi,uint32_t *data)
{
    if(!r || !lo || !hi || !data)return K70WifiItsInvalid;
    if(__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=K70WifiItsActive || !r->msi_iova)return K70WifiItsNotReady;
    *lo=(uint32_t)r->msi_iova;*hi=(uint32_t)(r->msi_iova>>32);*data=0;return 0;
}
extern "C" int k70_wifi_its_set_vector_enabled(K70WifiItsRuntime *r,uint32_t vector,int enabled)
{
    if(!r || vector>=K70WifiItsVectors || (enabled!=0 && enabled!=1))return K70WifiItsInvalid;
    if(__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=K70WifiItsActive)return K70WifiItsNotReady;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&r->command_busy,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return K70WifiItsBusy;
    uint8_t *prop=static_cast<uint8_t *>(r->memory[Properties].cpu);
    prop[r->lpi_base+vector-LpiFirst]=(uint8_t)(0xa2|enabled);
    int rc=r->ops.clean(r->ops.cookie,&r->memory[Properties]);
    if(!rc) {Command command{};command.word[0]=0x0c|(uint64_t)r->config.device_id<<32;command.word[1]=vector;rc=submit(r,command,true);}
    __atomic_store_n(&r->command_busy,0u,__ATOMIC_RELEASE);
    if(rc)k70_wifi_its_quarantine(r,rc);
    return rc;
}
extern "C" int k70_wifi_its_handle_irq(uint32_t intid)
{
    K70WifiItsRuntime *r=__atomic_load_n(&controller,__ATOMIC_ACQUIRE);
    if(!r || intid<r->lpi_base || intid-r->lpi_base>=r->vector_count)return 0;
    if(__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=K70WifiItsActive)return 1;
    const uint32_t vector=intid-r->lpi_base,bit=UINT32_C(1)<<vector;
    __atomic_fetch_add(&r->irq_count[vector],UINT64_C(1),__ATOMIC_RELAXED);
    const uint32_t previous=__atomic_fetch_or(&r->pending,bit,__ATOMIC_ACQ_REL);
    if(!previous)r->ops.wake_worker(r->ops.cookie);
    return 1;
}
extern "C" int k70_wifi_its_drain_pending(K70WifiItsRuntime *r)
{
    if(!r)return K70WifiItsInvalid;
    if(__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=K70WifiItsActive)return K70WifiItsNotReady;
    const uint32_t pending=__atomic_exchange_n(&r->pending,0u,__ATOMIC_ACQ_REL);
    for(unsigned vector=0;vector<K70WifiItsVectors;++vector)
        if(pending&(UINT32_C(1)<<vector))r->ops.deliver_irq(r->ops.cookie,(uint32_t)r->irq_base+vector);
    return pending?1:0;
}

#ifndef K70_WIFI_ITS_HOST_TEST
#include "NativeAdrenoMemory.h"
namespace {
struct NativePages { NativeAdrenoMemory memory; uint32_t used;bool held; };
NativePages native_pages[16]{};
}
extern "C" int k70_wifi_its_native_allocate(void *,size_t bytes,size_t alignment,K70WifiItsMemory *out)
{
    if(!out || !bytes || bytes>16*1024*1024 || alignment!=TableAlignment || bytes&(TableAlignment-1))return K70WifiItsInvalid;
    *out={};
    NativePages *slot=nullptr;
    for(auto &s:native_pages) {uint32_t expected=0;
        if(__atomic_compare_exchange_n(&s.used,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {slot=&s;break;}}
    if(!slot)return K70WifiItsNoMemory;
    const IOReturn status=slot->memory.allocateCpuPages(bytes+alignment,48,true);
    if(status) {
        const IOReturn cleanup=slot->memory.release();
        if(cleanup) {slot->memory.quarantine();out->owner=slot;return K70WifiItsIo;}
        __atomic_store_n(&slot->used,0u,__ATOMIC_RELEASE);return K70WifiItsNoMemory;
    }
    out->owner=slot;
    if(slot->memory.spanCount()!=1) {slot->memory.quarantine();return K70WifiItsUnsupported;}
    const NativeAdrenoPhysicalSpan &span=slot->memory.spans()[0];
    const uint64_t physical=(span.physical+alignment-1)&~(uint64_t)(alignment-1);
    const uint64_t offset=physical-span.physical;
    if(span.offset || offset+bytes>span.bytes || offset+bytes>slot->memory.ownedBytes()) {
        slot->memory.quarantine();return K70WifiItsUnsupported;
    }
    out->cpu=(uint8_t *)slot->memory.mapping()+offset;out->owner=slot;
    out->physical=physical;out->bytes=bytes;return 0;
}
extern "C" int k70_wifi_its_native_release(void *,K70WifiItsMemory *m)
{
    if(!m || !m->owner)return K70WifiItsInvalid;
    NativePages *slot=static_cast<NativePages *>(m->owner);
    /* release_memory is only called before the core publishes any BASER. */
    if(slot->held) {if(slot->memory.retireAfterQuiescence())return K70WifiItsIo;slot->held=false;}
    if(slot->memory.release())return K70WifiItsIo;
    __atomic_store_n(&slot->used,0u,__ATOMIC_RELEASE);*m={};return 0;
}
extern "C" int k70_wifi_its_native_hold(void *,K70WifiItsMemory *m)
{
    if(!m || !m->owner)return K70WifiItsInvalid;
    NativePages *slot=static_cast<NativePages *>(m->owner);
    if(slot->held)return K70WifiItsBusy;
    if(slot->memory.holdForDevice())return K70WifiItsIo;slot->held=true;return 0;
}
extern "C" void k70_wifi_its_native_quarantine(void *,K70WifiItsMemory *m)
{if(m && m->owner)static_cast<NativePages *>(m->owner)->memory.quarantine();}
extern "C" int k70_wifi_its_native_clean(void *,const K70WifiItsMemory *m)
{
    if(!m || !m->owner)return K70WifiItsInvalid;
    const NativePages *slot=static_cast<const NativePages *>(m->owner);
    return NativeAdrenoMaintainOwnedPages(slot->memory.mapping(),slot->memory.ownedBytes(),NativeAdrenoClean)?K70WifiItsIo:0;
}
extern "C" void k70_wifi_its_native_barrier(void *) {NativeAdrenoPublishBarrier();}
#endif
