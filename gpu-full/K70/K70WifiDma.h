/* SPDX-License-Identifier: MIT */
#ifndef K70_WIFI_DMA_H
#define K70_WIFI_DMA_H
#include <stddef.h>
#include <stdint.h>
enum {K70WifiDmaMaxBlocks=2048};
struct K70WifiDmaBlock {size_t offset,owned,requested;};
struct K70WifiDmaArena {
    uint8_t *cpu;uint64_t bus;size_t bytes,page,used;int quarantined;
    struct K70WifiDmaBlock blocks[K70WifiDmaMaxBlocks];size_t count;
    void *cookie;
    /* Native owned CPU pages, real cache operation and DMA barriers. Zero means success. */
    int (*sync_owned)(void *,void *,size_t,int);
};
/* Caller has already wired/zeroed exclusive CPU storage, installed and audited
 * an endpoint-reachable SMMU mapping, and retained both. This initializer cannot
 * establish PCIe/SMMU ownership or infer reachability from a physical address.
 * Serialize calls in the Wi-Fi dispatcher; retaining this arena is mandatory
 * until the endpoint is stopped and its SMMU translations are retired. */
int k70_wifi_dma_init(struct K70WifiDmaArena *,void *cpu,uint64_t bus,size_t bytes,
    size_t page,void *cookie,int (*sync_owned)(void *,void *,size_t,int));
void *k70_wifi_dma_alloc(struct K70WifiDmaArena *,size_t requested,uint64_t *bus);
/* Zero size and bus are the nw_abi lx_dma_kfree convention: resolve the exact
 * allocation from its base. The caller proves the device no longer uses it.
 * Failure/quarantine never makes the pages reusable. */
int k70_wifi_dma_free(struct K70WifiDmaArena *,size_t requested,void *cpu,uint64_t bus);
uint64_t k70_wifi_dma_map(struct K70WifiDmaArena *,void *cpu,size_t requested,int direction);
int k70_wifi_dma_sync(struct K70WifiDmaArena *,uint64_t bus,size_t requested,int direction);
void k70_wifi_dma_quarantine(struct K70WifiDmaArena *);
#endif
