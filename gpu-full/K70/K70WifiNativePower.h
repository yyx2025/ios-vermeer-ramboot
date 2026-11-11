/* SPDX-License-Identifier: MIT */
#ifndef K70_WIFI_NATIVE_POWER_H
#define K70_WIFI_NATIVE_POWER_H
#include <stddef.h>
#include <stdint.h>

enum K70WifiPowerRegion : unsigned {
    K70PowerRsc, K70PowerGcc, K70PowerTlmm, K70PowerClkref,
    K70PowerParf, K70PowerPhy, K70PowerDbi, K70PowerIatu,
    K70PowerConfig, K70PowerRegionCount
};
struct K70WifiPowerMapping {
    volatile uint8_t *cpu;
    uint64_t physical;
    size_t bytes;
};
/* Filled by the native shared-controller owner, never by Android snapshots.
 * The owner reserves RC0, its GPIOs/clocks/resets and one ACTIVE DRV2 TCS.
 * It also owns the shared-regulator/interconnect/mailbox votes. acquire/lock
 * must establish and check those reservations against every native consumer,
 * including touch RPMh requests. Merely finding an idle TCS is insufficient. */
struct K70WifiPowerLease {
    void *owner_cookie;
    K70WifiPowerMapping mmio[K70PowerRegionCount];
    const uint8_t *command_db;
    size_t command_db_bytes;
    unsigned active_tcs;
};
struct K70WifiPowerPlatform {
    void *cookie;
    int (*acquire)(void *,K70WifiPowerLease *);
    int (*lock)(void *,const K70WifiPowerLease *);
    void (*unlock)(void *,const K70WifiPowerLease *);
    /* Under the global sleepable lock: stop/drain native or firmware RC0
     * masters, validate cold ownership, establish PHY/CX/MX/NoC votes and
     * the 32-kHz clock, and acknowledge every supplied WLAN PDC message.
     * Must not return success just because another OS left hardware enabled. */
    int (*prepare_shared)(void *,const K70WifiPowerLease *,
        const char *const *pdc_messages,size_t count);
    void (*delay_us)(void *,unsigned);
    /* Device MMIO operations with real barriers and access-width semantics.
     * Native implementations may use k70_wifi_power_mmio_{read,write}. */
    int (*read)(void *,const K70WifiPowerMapping *,unsigned,unsigned,uint32_t *);
    int (*write)(void *,const K70WifiPowerMapping *,unsigned,unsigned,uint32_t);
};
enum K70WifiPowerStage : unsigned {
    K70PowerCold, K70PowerAcquiring, K70PowerShared, K70PowerRails,
    K70PowerPins, K70PowerDomains, K70PowerClocks, K70PowerController,
    K70PowerPhyReady, K70PowerLink, K70PowerEndpoint, K70PowerBar,
    K70PowerReady, K70PowerQuarantined
};
struct K70WifiPowerReport {
    unsigned stage,failed_stage;
    int error;
    uint32_t rsc_version,rail_completed_mask,endpoint_id;
    uint16_t msi_cap,pcie_cap;
    uint64_t bar_physical;
    size_t bar_bytes;
    bool lease_retained,mutation_attempted,bus_master_enabled;
};
class K70WifiNativePower final {
    K70WifiPowerPlatform platform_{};
    K70WifiPowerLease lease_{};
    K70WifiPowerReport report_{};
    unsigned gate_=0;
    struct RscLayout {unsigned control,status,enable,msg,address,data,command_status;};
    RscLayout rsc_{};
    int read(K70WifiPowerRegion,unsigned,uint32_t *,unsigned=4);
    int write(K70WifiPowerRegion,unsigned,uint32_t,unsigned=4);
    int change(K70WifiPowerRegion,unsigned,uint32_t,uint32_t);
    int wait(K70WifiPowerRegion,unsigned,uint32_t,uint32_t,unsigned);
    int send(uint32_t,uint32_t);
    int lookup(const char *,uint32_t *);
    int pin(unsigned,uint32_t,int);
    int atu(unsigned,uint64_t,uint64_t,uint64_t,unsigned);
    int discover();
    int fail(int);
public:
    K70WifiNativePower()=default;
    ~K70WifiNativePower()=default; /* no implicit unmap after an uncertain write */
    K70WifiNativePower(const K70WifiNativePower &)=delete;
    K70WifiNativePower &operator=(const K70WifiNativePower &)=delete;
    /* Train/enumerate with endpoint bus mastering DISABLED. Native DMA and
     * ITS owners must finish before enableBusMaster is called. */
    int start(const K70WifiPowerPlatform *);
    int configRead(int offset,int bytes,uint32_t *);
    int configWrite(int offset,int bytes,uint32_t);
    int enableBusMaster();
    int stopBusMaster();
    const K70WifiPowerReport *report() const {return &report_;}
};
extern "C" int k70_wifi_power_mmio_read(void *,const K70WifiPowerMapping *,
    unsigned offset,unsigned bytes,uint32_t *);
extern "C" int k70_wifi_power_mmio_write(void *,const K70WifiPowerMapping *,
    unsigned offset,unsigned bytes,uint32_t);
#endif
