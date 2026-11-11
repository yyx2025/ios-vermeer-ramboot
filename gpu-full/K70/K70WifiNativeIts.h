/* SPDX-License-Identifier: MIT */
#ifndef K70_WIFI_NATIVE_ITS_H
#define K70_WIFI_NATIVE_ITS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { K70WifiItsVectors=32, K70WifiItsMemoryCount=6, K70WifiItsMaxIdBits=24 };
enum { K70WifiItsMmio=1, K70WifiItsRedistributor=2 };
enum { K70WifiItsCold=0, K70WifiItsPreparing=1, K70WifiItsActive=2,
       K70WifiItsQuarantined=3, K70WifiItsReleased=4 };
enum { K70WifiItsInvalid=-22, K70WifiItsBusy=-16, K70WifiItsMissing=-2,
       K70WifiItsNoMemory=-12, K70WifiItsIo=-5, K70WifiItsTimeout=-110,
       K70WifiItsUnsupported=-95, K70WifiItsNotReady=-11 };
/* All physical ranges are separately retained, wired, and exclusively owned.
 * cpu/physical may select an aligned slice; owner retains its whole allocation. */
struct K70WifiItsMemory { void *cpu,*owner; uint64_t physical; size_t bytes; };
struct K70WifiItsConfig {
    uint64_t its_physical,redistributor_physical,timeout_ns;
    uint32_t cpu_number,affinity,device_id;
    uint16_t collection_id;
};
struct K70WifiItsOps {
    void *cookie;
    /* The global native GIC/resource manager must reserve the complete cold
     * ITS, this CPU's physical LPI tables and a 32-aligned range of 32 LPIs.
     * It also pins the target CPU online. It MUST NOT certify Android state,
     * select a Linux LPI number, or steal another native client's tables.
     * claim's lease remains held after every published-table failure. */
    int (*claim_global)(void *,const struct K70WifiItsConfig *,void **lease,
                        uint32_t *lpi_base,int32_t *irq_base);
    int (*release_unpublished)(void *,void *lease);
    int (*allocate)(void *,size_t bytes,size_t alignment,struct K70WifiItsMemory *);
    int (*release_memory)(void *,struct K70WifiItsMemory *);
    int (*hold_memory)(void *,struct K70WifiItsMemory *);
    void (*quarantine_memory)(void *,struct K70WifiItsMemory *);
    /* clean reaches PoC on every CPU alias of complete owned pages. */
    int (*clean)(void *,const struct K70WifiItsMemory *);
    void (*barrier)(void *);
    int (*read32)(void *,unsigned space,uint32_t off,uint32_t *);
    int (*read64)(void *,unsigned space,uint32_t off,uint64_t *);
    int (*write32)(void *,unsigned space,uint32_t off,uint32_t);
    int (*write64)(void *,unsigned space,uint32_t off,uint64_t);
    uint64_t (*now_ns)(void *);
    void (*relax)(void *);
    /* Maps this physical 4K Device-nGnRE/write-only doorbell page through the
     * native SID0x1401 domain. The lease/map remain pinned until reboot. */
    int (*map_msi_doorbell)(void *,uint64_t physical,size_t bytes,uint64_t *iova);
    /* IRQ context only signals a real worker. deliver_irq is called later,
     * serially from drain_pending, never directly in exception context. */
    void (*wake_worker)(void *);
    void (*deliver_irq)(void *,uint32_t irq);
};
struct K70WifiItsRuntime {
    uint32_t state,command_busy,pending,published;
    int last_error;
    uint32_t lpi_base,vector_count,id_bits,writer;
    int32_t irq_base;
    uint64_t target,msi_iova,typer;
    void *lease;
    struct K70WifiItsConfig config;
    struct K70WifiItsOps ops;
    struct K70WifiItsMemory memory[K70WifiItsMemoryCount];
    uint64_t irq_count[K70WifiItsVectors];
};
/* Pure board/config check. Native boot-CPU route is then checked at init. */
int k70_wifi_its_check_config(const struct K70WifiItsConfig *);
/* Static zeroed runtime. Enables only genuinely cold ITS/GICR; refuses live
 * inherited tables. No post-publication teardown or memory recycling exists.
 * Success means completed native setup, not that a real IRQ has arrived. */
int k70_wifi_its_init(struct K70WifiItsRuntime *,const struct K70WifiItsConfig *,
                      const struct K70WifiItsOps *);
int k70_wifi_its_msi_target(const struct K70WifiItsRuntime *,uint32_t *lo,
                           uint32_t *hi,uint32_t *data);
int k70_wifi_its_set_vector_enabled(struct K70WifiItsRuntime *,uint32_t vector,int enabled);
int k70_wifi_its_drain_pending(struct K70WifiItsRuntime *);
void k70_wifi_its_quarantine(struct K70WifiItsRuntime *,int error);
/* SWEET PE calls record_pe after the real CPU GIC interface is enabled.
 * Both hooks are supplied by the accompanying XNU patch. The strong hook
 * version symbol intentionally prevents a link with an unpatched IRQ path. */
void k70_wifi_its_record_pe(uint64_t redistributor_physical,uint32_t cpu_number,uint32_t affinity);
int k70_wifi_its_pe_route(uint32_t cpu_number,uint64_t *physical,uint32_t *affinity);
uint32_t k70_wifi_its_irq_hook_version(void);
/* Called with the actual ICC_IAR1 INTID. sleh_irq owns acknowledgement/EOI. */
int k70_wifi_its_handle_irq(uint32_t intid);
/* Concrete native XNU wired-page/PoC adapter, independent of GPU ownership.
 * Available in kernel builds (not K70_WIFI_ITS_HOST_TEST). */
int k70_wifi_its_native_allocate(void *,size_t,size_t,struct K70WifiItsMemory *);
int k70_wifi_its_native_release(void *,struct K70WifiItsMemory *);
int k70_wifi_its_native_hold(void *,struct K70WifiItsMemory *);
void k70_wifi_its_native_quarantine(void *,struct K70WifiItsMemory *);
int k70_wifi_its_native_clean(void *,const struct K70WifiItsMemory *);
void k70_wifi_its_native_barrier(void *);
#ifdef __cplusplus
}
#endif
#endif
