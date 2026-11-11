/* SPDX-License-Identifier: MIT */
#ifndef K70_WIFI_NATIVE_DMA_H
#define K70_WIFI_NATIVE_DMA_H
#include "K70WifiDma.h"
#include "NativeAdrenoMemory.h"
#include "ArmSmmu4KMmio.h"
#include "ArmSmmu4KStreams.h"
#include "ArmSmmu4KTableMemory.h"

/* A view of resources allocated by the native apps-SMMU owner. This is NOT a
 * caller-declared lease or a token granting hardware access. acquire/lock must
 * use the platform's actual exclusive resource allocator/control domains.
 * The allocator selects a reserved bank/ASID/exact SMR slot and IOVA windows;
 * neither this class nor the caller scans Android state for unused resources.
 * owner_cookie stays owned until release_lease actually succeeds. */
struct K70WifiNativeDmaLease {
    void *owner_cookie;
    volatile uint8_t *smmu_mapping;
    size_t smmu_bytes;
    uint64_t smmu_physical;
    uint64_t dma_iova,msi_iova;
    size_t dma_bytes;
    uint32_t sid,bank,asid,stream_slot;
    /* Derived from the native platform's table-walk coherence/ACTLR policy. */
    uint32_t coherent_walk;
};
struct K70WifiNativeDmaPlatform {
    void *cookie;
    /* Retain exclusive native SID0x1401, CB/ASID/SMR, IOVA and powered Device
     * MMIO resources. A failure may retain resources: leave owner_cookie set
     * when cleanup is uncertain. MSI page is separately owned Device MMIO,
     * exactly GITS_TRANSLATER's page0x17150000; it is never wired DRAM. */
    IOReturn (*acquire)(void *,uint32_t sid,size_t bytes,K70WifiNativeDmaLease *);
    /* Acquire the global configuration lock and VERIFY this owner still owns
     * these resources, that native power/clock domains and real mappings are
     * live, and no unrelated master shares the bank. Failure acquires no lock. */
    IOReturn (*lock)(void *,const K70WifiNativeDmaLease *);
    void (*unlock)(void *,const K70WifiNativeDmaLease *);
    /* Under lock: install owned global/context fault IRQ handlers, native
     * ACTLR/security/SCM/TCU/TBU requirements and fault-on-unmatched policy.
     * Complete and read back those operations; a boolean/token is insufficient. */
    IOReturn (*prepare_platform)(void *,const K70WifiNativeDmaLease *);
    /* Under lock: stop endpoint DMA/bus mastering, drain PCIe/TBU transactions
     * and confirm the hardware stopped. verify_quiesced rereads actual state;
     * neither callback may equate a memory reference or timeout with a fence. */
    IOReturn (*quiesce)(void *,const K70WifiNativeDmaLease *);
    IOReturn (*verify_quiesced)(void *,const K70WifiNativeDmaLease *);
    /* Under lock, only after faulted routes, disabled synchronized CB and page
     * retirement: release actual allocator reservations. MMIO/control-domain
     * references stay pinned through unlock. Failure retains the lease. */
    IOReturn (*release_lease)(void *,const K70WifiNativeDmaLease *);
};
enum K70WifiNativeDmaState : uint32_t {
    K70WifiNativeDmaCold=0,K70WifiNativeDmaClaiming,K70WifiNativeDmaActive,
    K70WifiNativeDmaQuarantined,K70WifiNativeDmaStopped,K70WifiNativeDmaReleased
};
struct K70WifiNativeDmaReport {
    uint32_t state,stage;
    IOReturn status;
    ArmSmmu4KStatus smmu_status;
    uint32_t id0,id1,id2,bank,asid,stream_slot;
    uint64_t dma_iova,dma_physical,table_physical,msi_iova;
    size_t arena_bytes,cpu_page_bytes,table_pages;
    uint32_t doorbell_references;
    bool lease_retained,context_attempted,context_disabled,streams_faulted;
    bool software_mapping_ready;
    /* Code execution is not device validation. Never set by this owner. */
    bool hardware_qualified;
};

/* One-shot owned Wi-Fi address space, including an independently owned ITS
 * Device-nGnRE doorbell leaf. All methods serialize through the platform lock
 * plus this object's gate. CPU arena calls are serialized by K70's dispatcher.
 * Object, platform cookies and output arena must remain pinned after ANY claim
 * attempt until release succeeds. No destructor unmaps or releases resources.
 * Cache callbacks use real NativeAdrenoMemory CPU-page primitives only; no GPU
 * SMMU policy, Android TTBR, UFS extra arena or caller-supplied cache callback. */
class K70WifiNativeDma final {
    K70WifiNativeDmaPlatform platform_{};
    K70WifiNativeDmaLease lease_{};
    NativeAdrenoMemory data_,tables_;
    ArmSmmu4KMmio mmio_{};
    ArmSmmu4KIo io_{};
    ArmSmmu4KConfig table_config_{};
    ArmSmmu4KResult table_result_{};
    ArmSmmu4KContextConfig context_{};
    ArmSmmu4KStream route_{};
    ArmSmmu4KStreamState streams_{};
    ArmSmmu4KRange ranges_[2]{};
    ArmSmmu4KTableMemory table_memory_{};
    ArmSmmu4KTableIo table_io_{};
    ArmSmmu4KAuditRegion audit_region_{};
    ArmSmmu4KAuditConfig audit_config_{};
    unsigned char visited_[128]{};
    K70WifiDmaArena *arena_=nullptr;
    K70WifiNativeDmaReport report_{};
    uint32_t gate_=0;
    bool data_held_=false,tables_held_=false,programmed_=false;
    bool attach_attempted_=false,attached_=false,platform_locked_=false;
    IOReturn enter();
    void leave();
    IOReturn fail(IOReturn,ArmSmmu4KStatus=ArmSmmu4KSuccess);
    IOReturn stopLocked();
    IOReturn observeLocked(bool audit_tables);
    static int syncOwned(void *,void *,size_t,int);
public:
    enum : size_t {MaxArenaBytes=64u*1024u*1024u};
    enum : uint64_t {AppsSmmuPhysical=0x15000000,ItsDoorbellPhysical=0x17150000};
    K70WifiNativeDma()=default;
    ~K70WifiNativeDma()=default;
    K70WifiNativeDma(const K70WifiNativeDma &)=delete;
    K70WifiNativeDma &operator=(const K70WifiNativeDma &)=delete;
    /* Builds/audits inactive tables, publishes real cacheable CPU backing,
     * programs only an owned disabled CB, and attaches exact SID0x1401 while
     * the endpoint remains stopped. Success leaves endpoint startup to PCIe. */
    IOReturn claim(const K70WifiNativeDmaPlatform *,uint32_t sid,size_t bytes,
        K70WifiDmaArena *);
    /* Full table/route observation requires real quiescence. */
    IOReturn observe();
    /* Pin the prebuilt separately owned Device leaf. Called by ITS before MSI
     * capability publication; the offset0x40 is added by ITS. No RAM alias. */
    IOReturn mapMsiDoorbell(uint64_t physical,size_t bytes,uint64_t *iova);
    /* stop() must precede ITS teardown/unmap: it stops DMA, faults streams and
     * disables/synchronizes the CB. Only then can a doorbell pin be retired. */
    IOReturn unmapMsiDoorbell(uint64_t physical,size_t bytes,uint64_t iova);
    IOReturn stop();
    /* After stop, MSI pins retired and independently reverified quiescence:
     * invalidate device writes, retire holds, complete/release wired pages,
     * then release the lease. Failures retain all uncertain resources. */
    IOReturn release();
    const K70WifiNativeDmaReport *report() const {return &report_;}
};
#endif
