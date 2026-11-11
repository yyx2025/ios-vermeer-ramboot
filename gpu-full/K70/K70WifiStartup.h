/* SPDX-License-Identifier: MIT */
#ifndef K70_WIFI_STARTUP_H
#define K70_WIFI_STARTUP_H
#include "K70WifiDma.h"
#include "nw_abi.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {K70WifiFirmwareMax=16, K70WifiNameMax=64};
enum {K70WifiIdentityEndpoint=1, K70WifiIdentityChip=2,
      K70WifiIdentityBoard=4, K70WifiIdentityCountry=8, K70WifiIdentityAll=15};
enum {K70WifiAmss=1, K70WifiPhy=2, K70WifiCalibration=3,
      K70WifiRegdb=4, K70WifiMac=5};
enum {K70WifiCold=0, K70WifiStarting=1, K70WifiStarted=2,
      K70WifiQuarantined=3};
enum {K70WifiInvalid=-22, K70WifiMissing=-2, K70WifiBusy=-16,
      K70WifiWrongDevice=-19, K70WifiUnqualified=-78, K70WifiIntegrity=-84};
struct K70WifiIdentity {
    uint32_t known,chip_id,chip_revision,board_id;
    uint16_t vendor,device;
    char country[2];
};
/* A request alias is an explicit, format-qualified translation for the exact
 * driver revision, not a guessed conversion from vendor ELF to board.bin.
 * RAM bytes and all callback cookies are pinned until reboot. */
struct K70WifiFirmware {
    char request[K70WifiNameMax],source[K70WifiNameMax];
    uint32_t role;
    const uint8_t *data;size_t bytes;uint8_t sha256[32];
};
struct K70WifiSelection {
    struct K70WifiIdentity identity;
    uint8_t stock_module_sha256[32],selection_evidence_sha256[32];
    uint8_t driver_format_evidence_sha256[32];
    struct K70WifiFirmware files[K70WifiFirmwareMax];size_t count;
};
struct K70WifiBackend {
    void *cookie;
    struct lx_platform platform;
    /* Native implementations must own their MMIO, power, PCIe root complex,
     * translated SID 0x1401 DMA pages and MSI resources, including failures.
     * No callback may return success on the basis of stock Android leftovers. */
    int (*power_and_pcie)(void *,struct nw_board *);
    int (*claim_dma)(void *,uint32_t sid,struct K70WifiDmaArena *);
    int (*attach_msi)(void *,const struct nw_board *);
    /* Called when calibration/regdb is requested. It reads the running
     * firmware's QMI chip/board identity and qualified country selection. */
    int (*firmware_identity)(void *,struct K70WifiIdentity *);
    void (*wake_dispatcher)(void *);
    int (*publish_network)(void *,const uint8_t mac[6]);
};
struct K70WifiRuntime {
    uint32_t state,dispatch_busy;
    int last_error,firmware_error,network_published,calibration_served;
    struct K70WifiSelection selection;
    struct K70WifiBackend backend;
    struct lx_platform platform;
    struct nw_board board;
    struct K70WifiDmaArena dma;
};
/* Pure preflight: no power writes, globals or allocations. Hashes all RAM
 * files and refuses missing identity/provenance, duplicates and unknown roles. */
int k70_wifi_qualify_selection(const struct K70WifiSelection *);
/* Pure provider; protected files require a matching current firmware tuple.
 * No path traversal, implicit fallback or unqualified country choice. */
int k70_wifi_ram_get(const struct K70WifiSelection *,const struct K70WifiIdentity *,
    const char *request,const uint8_t **data,size_t *bytes);
/* One-shot singleton matching nw_abi's global dispatcher. Runtime must be
 * zero-initialized static storage. Every post-power failure quarantines DMA;
 * runtime, firmware, mappings and callbacks remain pinned until reboot. */
int k70_wifi_startup(struct K70WifiRuntime *,const struct K70WifiBackend *,
    const struct K70WifiSelection *);
int k70_wifi_dispatch(struct K70WifiRuntime *);
/* WifiNet calls this after queueing TX. It signals the installed real worker;
 * it never runs the wireless driver concurrently on the caller's thread. */
void sheng_wifi_wake(void);
#ifdef __cplusplus
}
#endif
#endif
