/* SPDX-License-Identifier: MIT */
#include "K70WifiNativePower.h"
#include "K70WifiPowerBoard.h"
#include <string.h>

namespace {
enum {Invalid=-22,Busy=-16,Missing=-2,Io=-5,Timeout=-60,Unqualified=-78};
const uint64_t physical[K70PowerRegionCount]={0x17a20000,0x100000,0xf100000,
    0x1fd5000,0x1c00000,0x1c06000,0x60000000,0x60001000,0x60100000};
const size_t minimum[K70PowerRegionCount]={0x10000,0x70000,0x61000,
    0x1000,0x3000,0x2000,0xf1d,0x1000,0x100000};
bool access(const K70WifiPowerMapping *m,unsigned off,unsigned bytes) {
    return m && m->cpu && (bytes==1 || bytes==2 || bytes==4) &&
        !(off&(bytes-1)) && off<=m->bytes && bytes<=m->bytes-off;
}
void deviceBarrier() {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
uint16_t u16(const uint8_t *p) {return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t *p) {return uint32_t(u16(p))|(uint32_t(u16(p+2))<<16);}
uint8_t ridHash(uint16_t rid) {
    uint8_t crc=0;
    for(unsigned b=0;b<2;++b) {
        crc^=uint8_t(rid>>(b?0:8));
        for(unsigned i=0;i<8;++i)crc=uint8_t((crc<<1)^((crc&0x80)?7:0));
    }
    return crc;
}
}
extern "C" int k70_wifi_power_mmio_read(void *,const K70WifiPowerMapping *m,
    unsigned off,unsigned bytes,uint32_t *v) {
    if(!v || !access(m,off,bytes))return Invalid;
    deviceBarrier();
    if(bytes==1)*v=*(m->cpu+off);
    else if(bytes==2)*v=*reinterpret_cast<volatile uint16_t *>(m->cpu+off);
    else *v=*reinterpret_cast<volatile uint32_t *>(m->cpu+off);
    deviceBarrier();return 0;
}
extern "C" int k70_wifi_power_mmio_write(void *,const K70WifiPowerMapping *m,
    unsigned off,unsigned bytes,uint32_t v) {
    if(!access(m,off,bytes))return Invalid;
    deviceBarrier();
    if(bytes==1)*(m->cpu+off)=uint8_t(v);
    else if(bytes==2)*reinterpret_cast<volatile uint16_t *>(m->cpu+off)=uint16_t(v);
    else *reinterpret_cast<volatile uint32_t *>(m->cpu+off)=v;
    deviceBarrier();return 0;
}
int K70WifiNativePower::read(K70WifiPowerRegion r,unsigned o,uint32_t *v,unsigned n) {
    return platform_.read(platform_.cookie,&lease_.mmio[r],o,n,v);
}
int K70WifiNativePower::write(K70WifiPowerRegion r,unsigned o,uint32_t v,unsigned n) {
    report_.mutation_attempted=true;
    return platform_.write(platform_.cookie,&lease_.mmio[r],o,n,v);
}
int K70WifiNativePower::change(K70WifiPowerRegion r,unsigned o,uint32_t clear,uint32_t set) {
    uint32_t v=0;int rc=read(r,o,&v);return rc?rc:write(r,o,(v&~clear)|set);
}
int K70WifiNativePower::wait(K70WifiPowerRegion r,unsigned o,uint32_t mask,
    uint32_t expected,unsigned budget_us) {
    for(unsigned elapsed=0;elapsed<=budget_us;elapsed+=10) {
        uint32_t v=0;int rc=read(r,o,&v);if(rc)return rc;
        if((v&mask)==expected)return 0;
        if(elapsed<budget_us)platform_.delay_us(platform_.cookie,10);
    }
    return Timeout;
}
int K70WifiNativePower::fail(int rc) {
    report_.failed_stage=report_.stage;report_.stage=K70PowerQuarantined;
    report_.error=rc;return rc;
}
/* cmd-db tables are byte-oriented, bounded and pinned by the resource owner.
 * Resource addresses come from the running native command database. */
int K70WifiNativePower::lookup(const char *name,uint32_t *address) {
    const auto *db=lease_.command_db;const size_t bytes=lease_.command_db_bytes;
    if(!name || !address || !db || bytes<144 || u32(db+4)!=0x0c0330db)return Unqualified;
    const size_t length=strlen(name);if(!length || length>8)return Invalid;
    uint8_t id[8]={};memcpy(id,name,length);unsigned found=0;
    for(unsigned table=0;table<8;++table) {
        const auto *h=db+8+16*table;if(!u16(h))continue;
        const size_t offset=144+u16(h+2),count=u16(h+6);
        if(offset>bytes || count>(bytes-offset)/24)return Unqualified;
        for(size_t i=0;i<count;++i) {
            const auto *entry=db+offset+24*i;
            if(!memcmp(entry,id,8)) {
                const uint32_t a=u32(entry+16);
                if(!a || (a&3) || ((a>>16)&7)!=4 || a>UINT32_MAX-8)return Unqualified;
                *address=a;++found;
            }
        }
    }
    return found==1?0:found?Unqualified:Missing;
}
/* Use one command in an explicitly leased ACTIVE TCS. A timeout never clears
 * enable/control or releases that TCS. Both the IRQ latch and command response
 * must confirm completion; a fixed delay is not treated as acknowledgement. */
int K70WifiNativePower::send(uint32_t address,uint32_t data) {
    const unsigned base=0xd00+672*lease_.active_tcs,bit=1u<<lease_.active_tcs;
    uint32_t enabled=0,status=0;
    int rc=read(K70PowerRsc,base+rsc_.enable,&enabled);if(rc)return rc;
    rc=read(K70PowerRsc,base+rsc_.status,&status);if(rc)return rc;
    if(enabled || !(status&1))return Busy;
    if((rc=write(K70PowerRsc,0xd08,bit)) ||
       (rc=write(K70PowerRsc,base+rsc_.msg,0x10108)) ||
       (rc=write(K70PowerRsc,base+rsc_.address,address)) ||
       (rc=write(K70PowerRsc,base+rsc_.data,data)) ||
       (rc=change(K70PowerRsc,0xd00,0,bit)) ||
       (rc=write(K70PowerRsc,base+rsc_.enable,1)) ||
       (rc=change(K70PowerRsc,base+rsc_.control,0x1010000,0)) ||
       (rc=write(K70PowerRsc,base+rsc_.control,0x10000)) ||
       (rc=write(K70PowerRsc,base+rsc_.control,0x1010000)))return rc;
    rc=wait(K70PowerRsc,0xd04,bit,bit,200000);if(rc)return rc;
    rc=read(K70PowerRsc,base+rsc_.command_status,&status);if(rc)return rc;
    if(!(status&0x10000))return Io;
    if((rc=write(K70PowerRsc,base+rsc_.enable,0)) ||
       (rc=write(K70PowerRsc,0xd08,bit)))return rc;
    return 0;
}
int K70WifiNativePower::pin(unsigned number,uint32_t cfg,int level) {
    const unsigned off=number*0x1000;int rc=0;
    if(level>=0 && (rc=write(K70PowerTlmm,off+4,level?2:0)))return rc;
    if((rc=write(K70PowerTlmm,off,cfg)))return rc;
    if(level>=0 && (rc=write(K70PowerTlmm,off+4,level?2:0)))return rc;
    uint32_t v=0;if((rc=read(K70PowerTlmm,off,&v)))return rc;
    if((v&0x3ff)!=cfg)return Io;
    if(level>=0) {
        if((rc=read(K70PowerTlmm,off+4,&v)))return rc;
        if(bool(v&2)!=bool(level))return Io;
    }
    return 0;
}
int K70WifiNativePower::atu(unsigned index,uint64_t base,uint64_t bytes,
    uint64_t target,unsigned type) {
    if(index>1 || !bytes || base>UINT64_MAX-bytes || (base>>32)!=((base+bytes-1)>>32))return Invalid;
    const unsigned o=index*0x200;int rc=0;
    if((rc=write(K70PowerIatu,o+4,0)) ||
       (rc=write(K70PowerIatu,o+8,uint32_t(base))) ||
       (rc=write(K70PowerIatu,o+12,uint32_t(base>>32))) ||
       (rc=write(K70PowerIatu,o+16,uint32_t(base+bytes-1))) ||
       (rc=write(K70PowerIatu,o+20,uint32_t(target))) ||
       (rc=write(K70PowerIatu,o+24,uint32_t(target>>32))) ||
       (rc=write(K70PowerIatu,o,type)) ||
       (rc=write(K70PowerIatu,o+4,0x80000000)))return rc;
    return wait(K70PowerIatu,o+4,0x80000000,0x80000000,1000);
}
int K70WifiNativePower::discover() {
    uint32_t id=0,status=0,pointer=0;int rc=read(K70PowerConfig,0,&id);
    if(rc)return rc;report_.endpoint_id=id;if(id!=0x110717cb)return Unqualified;
    /* Bus mastering remains off during capability discovery and BAR probing. */
    uint32_t command=0;if((rc=read(K70PowerConfig,4,&command,2)))return rc;
    if(command&4)return Unqualified;
    if((rc=write(K70PowerConfig,4,command&~7u,2)) ||
       (rc=read(K70PowerConfig,6,&status,2)))return rc;
    if(!(status&0x10))return Unqualified;
    if((rc=read(K70PowerConfig,0x34,&pointer,1)))return rc;
    uint64_t seen=0;unsigned count=0;
    while(pointer) {
        if(pointer<0x40 || pointer>0xfc || (pointer&3) || count++==48)return Unqualified;
        const uint64_t bit=uint64_t(1)<<(pointer/4);if(seen&bit)return Unqualified;seen|=bit;
        uint32_t h=0;if((rc=read(K70PowerConfig,pointer,&h,2)))return rc;
        if((h&0xff)==5) {if(report_.msi_cap || pointer>0xec)return Unqualified;report_.msi_cap=uint16_t(pointer);}
        if((h&0xff)==0x10) {if(report_.pcie_cap || pointer>0xdc)return Unqualified;report_.pcie_cap=uint16_t(pointer);}
        pointer=(h>>8)&0xff;
    }
    if(!report_.msi_cap || !report_.pcie_cap)return Unqualified;
    uint32_t msi=0;if((rc=read(K70PowerConfig,report_.msi_cap+2,&msi,2)))return rc;
    if((msi&1) || ((msi>>1)&7)<5)return Unqualified;
    report_.stage=K70PowerBar;
    uint32_t original=0,high=0,mask=0,mask_high=0;
    if((rc=read(K70PowerConfig,0x10,&original)))return rc;
    const unsigned type=(original>>1)&3;if((original&1) || (type!=0&&type!=2))return Unqualified;
    if(type==2 && (rc=read(K70PowerConfig,0x14,&high)))return rc;
    if((rc=write(K70PowerConfig,0x10,0xffffffff)) ||
       (type==2 && (rc=write(K70PowerConfig,0x14,0xffffffff))) ||
       (rc=read(K70PowerConfig,0x10,&mask)) ||
       (type==2 && (rc=read(K70PowerConfig,0x14,&mask_high))))return rc;
    const uint64_t full=(uint64_t(type==2?mask_high:0xffffffff)<<32)|(mask&~0xfu);
    const uint64_t size=~full+1;
    if(!size || (size&(size-1)) || size>0x3d00000 || size<0x1000)return Unqualified;
    const uint64_t bar=(0x60300000ull+size-1)&~(size-1);
    if(bar<0x60300000 || bar>0x64000000-size)return Unqualified;
    if((rc=write(K70PowerConfig,0x10,uint32_t(bar)|(original&0xf))) ||
       (type==2 && (rc=write(K70PowerConfig,0x14,uint32_t(bar>>32)))) ||
       (rc=read(K70PowerConfig,0x10,&mask)) ||
       (type==2 && (rc=read(K70PowerConfig,0x14,&mask_high))))return rc;
    if((mask&~0xfu)!=uint32_t(bar) || (type==2&&mask_high!=uint32_t(bar>>32)))return Io;
    if((rc=write(K70PowerConfig,4,(command&~5u)|2u,2)) ||
       (rc=read(K70PowerConfig,4,&command,2)))return rc;
    if((command&7)!=2)return Io;
    report_.bar_physical=bar;report_.bar_bytes=size;return 0;
}
int K70WifiNativePower::start(const K70WifiPowerPlatform *p) {
    if(!p || !p->acquire || !p->lock || !p->unlock || !p->prepare_shared ||
       !p->read || !p->write || !p->delay_us)return Invalid;
    unsigned expected=0;if(!__atomic_compare_exchange_n(&gate_,&expected,1u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return Busy;
    if(report_.stage!=K70PowerCold) {__atomic_store_n(&gate_,0u,__ATOMIC_RELEASE);return Busy;}
    platform_=*p;report_.stage=K70PowerAcquiring;
    int rc=p->acquire(p->cookie,&lease_);report_.lease_retained=lease_.owner_cookie!=nullptr;
    if(rc || !lease_.owner_cookie) {fail(rc?rc:Unqualified);__atomic_store_n(&gate_,0u,__ATOMIC_RELEASE);return report_.error;}
    rc=p->lock(p->cookie,&lease_);
    if(rc) {fail(rc);__atomic_store_n(&gate_,0u,__ATOMIC_RELEASE);return rc;}
    auto run=[&]() -> int {
        if(lease_.active_tcs>=3 || !lease_.command_db || lease_.command_db_bytes!=0x20000)return Unqualified;
        for(unsigned i=0;i<K70PowerRegionCount;++i)
            if(!lease_.mmio[i].cpu || lease_.mmio[i].physical!=physical[i] || lease_.mmio[i].bytes<minimum[i])return Unqualified;
        report_.stage=K70PowerShared;
        int e=p->prepare_shared(p->cookie,&lease_,k70_wifi_pdc,sizeof(k70_wifi_pdc)/sizeof(k70_wifi_pdc[0]));if(e)return e;
        if((e=read(K70PowerRsc,0,&report_.rsc_version)))return e;
        const unsigned major=(report_.rsc_version>>16)&0xff,minor=(report_.rsc_version>>8)&0xff;
        if(major==3)rsc_={0x24,0x28,0x2c,0x34,0x38,0x3c,0x40};
        else if(major==2&&minor==7)rsc_={0x14,0x18,0x1c,0x30,0x34,0x38,0x3c};
        else return Unqualified;
        report_.stage=K70PowerRails;
        for(unsigned i=0;i<6;++i) {
            uint32_t address=0;if((e=lookup(k70_wifi_rails[i].resource,&address)))return e;
            const auto &rail=k70_wifi_rails[i];
            if(!rail.minimum_uv || rail.minimum_uv>rail.maximum_uv || rail.minimum_uv%1000)return Unqualified;
            if((e=send(address+8,7)) || (e=send(address,rail.minimum_uv/1000)) ||
               (e=send(address+4,1)))return e;
            if(rail.delay_us)p->delay_us(p->cookie,rail.delay_us);
            report_.rail_completed_mask|=1u<<i;
        }
        report_.stage=K70PowerPins;
        if((e=pin(94,0x201,0)) || (e=pin(95,7,-1)) || (e=pin(96,3,-1)) ||
           (e=pin(80,0x3c3,0)))return e;
        p->delay_us(p->cookie,150000);
        if((e=pin(80,0x3c3,1)))return e;p->delay_us(p->cookie,60000);
        report_.stage=K70PowerDomains;
        if((e=change(K70PowerGcc,0x52020,9,0)) ||
           (e=wait(K70PowerGcc,0x6b004,0x80000000,0x80000000,1500)) ||
           (e=wait(K70PowerGcc,0x6c000,0x80000000,0x80000000,1500)))return e;
        report_.stage=K70PowerClocks;
        if((e=write(K70PowerGcc,0x6b078,0)) || (e=change(K70PowerGcc,0x6b074,0,1)) ||
           (e=write(K70PowerGcc,0x6b05c,0x605)) || (e=change(K70PowerGcc,0x6b058,0,1)) ||
           (e=wait(K70PowerGcc,0x6b074,1,0,1000)) || (e=wait(K70PowerGcc,0x6b058,1,0,1000)) ||
           (e=change(K70PowerGcc,0x6b070,3,2)) || (e=change(K70PowerGcc,0x52008,0,0x7f)) ||
           (e=change(K70PowerGcc,0x52000,0,(1u<<12)|(1u<<19)|(1u<<22))) ||
           (e=change(K70PowerClkref,0x100,0,1)))return e;
        const unsigned clocks[]={0x6b020,0x6b02c,0x6b038,0x6b03c,0x6b01c,0x10030,0x1003c,0x1004c,0x6b054};
        for(unsigned o:clocks)if((e=wait(K70PowerGcc,o,0x80000000,0,1000)))return e;
        const unsigned resets[]={0x6b000u,0x6c01cu};
        for(unsigned o:resets) {
            if((e=change(K70PowerGcc,o,0,1)))return e;p->delay_us(p->cookie,1000);
            if((e=change(K70PowerGcc,o,1,0)))return e;p->delay_us(p->cookie,1000);
            if((e=wait(K70PowerGcc,o,1,0,1000)))return e;
        }
        report_.stage=K70PowerController;
        if((e=write(K70PowerParf,0x1000,4)) || (e=change(K70PowerParf,0x40,1,0)) ||
           (e=write(K70PowerParf,0x350,0x60000000)) || (e=write(K70PowerParf,0x354,0)) ||
           (e=write(K70PowerParf,0x634,0x60001000)) || (e=write(K70PowerParf,0x638,0)) ||
           (e=write(K70PowerParf,0x358,0)) || (e=write(K70PowerParf,0x35c,0x80000000)) ||
           (e=change(K70PowerParf,0,1u<<29,0)) || (e=change(K70PowerParf,0x174,0,1u<<4)) ||
           (e=change(K70PowerParf,0x20,1u<<5,0)) || (e=change(K70PowerParf,0x1a8,0,1u<<31)))return e;
        for(const auto &phy:k70_wifi_power_phy) {
            if((e=write(K70PowerPhy,phy.offset,phy.value)))return e;
            if(phy.delay_us)p->delay_us(p->cookie,phy.delay_us);
        }
        if((e=change(K70PowerGcc,0x6b070,3,0)) || (e=change(K70PowerPhy,0x200,1,0)) ||
           (e=change(K70PowerPhy,0x244,0,3)) || (e=wait(K70PowerPhy,0x214,1u<<6,0,10000)))return e;
        report_.stage=K70PowerPhyReady;
        for(unsigned i=0;i<256;++i)if((e=write(K70PowerParf,0x2000+4*i,0)))return e;
        if((e=write(K70PowerParf,0x2000+4*ridHash(0x100),0x01000100)) ||
           (e=change(K70PowerParf,0x2c00,1,0)) ||
           (e=write(K70PowerDbi,0x18,0x00010100)) || (e=write(K70PowerDbi,4,6,2)))return e;
        p->delay_us(p->cookie,100000);if((e=pin(94,0x201,1)))return e;p->delay_us(p->cookie,1000);
        if((e=change(K70PowerParf,0x1b0,0,1u<<8)))return e;
        report_.stage=K70PowerLink;
        if((e=wait(K70PowerDbi,0x72c,1u<<4,1u<<4,1000000)) ||
           (e=wait(K70PowerDbi,0x728,0x3f,0x11,10000)) ||
           (e=atu(0,0x60100000,0x100000,0x01000000,4)) ||
           (e=atu(1,0x60300000,0x3d00000,0x60300000,0)))return e;
        report_.stage=K70PowerEndpoint;return discover();
    };
    rc=run();if(rc)fail(rc);else report_.stage=K70PowerReady;
    p->unlock(p->cookie,&lease_);__atomic_store_n(&gate_,0u,__ATOMIC_RELEASE);return rc;
}
int K70WifiNativePower::configRead(int offset,int bytes,uint32_t *value) {
    if(!value || report_.stage!=K70PowerReady)return Unqualified;
    uint32_t off=uint32_t(offset);
    if(off&0x80000000u) {off&=0x7fffffffu;if(off>0x3c)return Invalid;off+=report_.pcie_cap;}
    if(off>=4096 || (bytes!=1&&bytes!=2&&bytes!=4) || (off&(bytes-1)) || unsigned(bytes)>4096-off)return Invalid;
    return read(K70PowerConfig,off,value,unsigned(bytes));
}
int K70WifiNativePower::configWrite(int offset,int bytes,uint32_t value) {
    if(report_.stage!=K70PowerReady)return Unqualified;
    uint32_t off=uint32_t(offset);
    if(off&0x80000000u) {off&=0x7fffffffu;if(off>0x3c)return Invalid;off+=report_.pcie_cap;}
    if(off>=4096 || (bytes!=1&&bytes!=2&&bytes!=4) || (off&(bytes-1)) || unsigned(bytes)>4096-off)return Invalid;
    /* Bus mastering is owned by enable/stopBusMaster, after DMA/ITS readiness. */
    if(off<=4 && off+unsigned(bytes)>4 && (value&(4u<<((4-off)*8))))return Unqualified;
    return write(K70PowerConfig,off,value,unsigned(bytes));
}
int K70WifiNativePower::enableBusMaster() {
    if(report_.stage!=K70PowerReady)return Unqualified;
    int rc=platform_.lock(platform_.cookie,&lease_);if(rc)return rc;
    uint32_t command=0;rc=read(K70PowerConfig,4,&command,2);
    if(!rc)rc=write(K70PowerConfig,4,command|6u,2);
    if(!rc)rc=read(K70PowerConfig,4,&command,2);
    if(!rc&&(command&6)!=6)rc=Io;
    report_.bus_master_enabled=rc!=0 || bool(command&4);
    platform_.unlock(platform_.cookie,&lease_);return rc?fail(rc):0;
}
int K70WifiNativePower::stopBusMaster() {
    if(!lease_.owner_cookie)return Unqualified;
    int rc=platform_.lock(platform_.cookie,&lease_);if(rc)return rc;
    uint32_t command=0;rc=read(K70PowerConfig,4,&command,2);
    if(!rc)rc=write(K70PowerConfig,4,command&~4u,2);
    if(!rc)rc=read(K70PowerConfig,4,&command,2);
    if(!rc&&(command&4))rc=Io;
    /* PCIe transactions can remain in flight. This only disables new mastering;
     * DMA platform quiesce must additionally drain TBU/PCIe transactions. */
    if(!rc)report_.bus_master_enabled=false;
    platform_.unlock(platform_.cookie,&lease_);return rc;
}
