/* SPDX-License-Identifier: MIT */
#include "K70WifiStartup.h"
#include "NativeAdrenoSha256.h"
#include <string.h>
namespace {
K70WifiRuntime *installed;
const uint8_t stockModule[32]={0xe0,0x2d,0xff,0x73,0x76,0xec,0x96,0x67,
    0x14,0x54,0x20,0x9a,0x28,0x3a,0x1d,0xdd,0xd3,0x93,0x7f,0xdb,0xe6,0x8e,
    0x3f,0x25,0xea,0x37,0xbe,0x9b,0xa3,0x5d,0x35,0x28};
bool digestPresent(const uint8_t *p) {uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
bool nameValid(const char *s) {
    size_t n=0;for(;n<K70WifiNameMax && s[n];++n)
        if(!((s[n]>='a'&&s[n]<='z')||(s[n]>='A'&&s[n]<='Z')||
             (s[n]>='0'&&s[n]<='9')||s[n]=='_'||s[n]=='-'||s[n]=='.'))return false;
    return n && n<K70WifiNameMax && !strstr(s,"..");
}
bool identityValid(const K70WifiIdentity &id) {
    return id.known==K70WifiIdentityAll && id.vendor==0x17cb && id.device==0x1107 &&
        id.country[0]>='A' && id.country[0]<='Z' && id.country[1]>='A' && id.country[1]<='Z';
}
bool sameIdentity(const K70WifiIdentity &a,const K70WifiIdentity &b) {
    return identityValid(a)&&identityValid(b)&&a.vendor==b.vendor&&a.device==b.device&&
        a.chip_id==b.chip_id&&a.chip_revision==b.chip_revision&&a.board_id==b.board_id&&
        a.country[0]==b.country[0]&&a.country[1]==b.country[1];
}
bool fileHash(const K70WifiFirmware &f) {
    uint8_t hash[32];NativeAdrenoSha256::hash(f.data,f.bytes,hash);
    return !memcmp(hash,f.sha256,32);
}
int fail(K70WifiRuntime *r,int code) {
    r->last_error=code;k70_wifi_dma_quarantine(&r->dma);
    __atomic_store_n(&r->state,uint32_t(K70WifiQuarantined),__ATOMIC_RELEASE);return code;
}
K70WifiRuntime *current() {return __atomic_load_n(&installed,__ATOMIC_ACQUIRE);}
void *dmaAlloc(size_t n,uint64_t *bus) {
    auto *r=current();if(!r)return nullptr;
    void *cpu=k70_wifi_dma_alloc(&r->dma,n,bus);
    if(r->dma.quarantined)r->firmware_error=K70WifiIntegrity;
    return cpu;
}
void dmaFree(size_t n,void *cpu,uint64_t bus) {
    auto *r=current();if(r && k70_wifi_dma_free(&r->dma,n,cpu,bus)) {
        r->firmware_error=K70WifiIntegrity;k70_wifi_dma_quarantine(&r->dma);
    }
}
uint64_t dmaMap(void *cpu,size_t n,int dir) {
    auto *r=current();if(!r)return 0;
    uint64_t bus=k70_wifi_dma_map(&r->dma,cpu,n,dir);
    if(r->dma.quarantined)r->firmware_error=K70WifiIntegrity;
    return bus;
}
void dmaSync(uint64_t bus,size_t n,int dir) {
    auto *r=current();if(r && k70_wifi_dma_sync(&r->dma,bus,n,dir)) {
        r->firmware_error=K70WifiIntegrity;k70_wifi_dma_quarantine(&r->dma);
    }
}
int fwGet(const char *name,const uint8_t **data,size_t *bytes) {
    auto *r=current();if(!r)return K70WifiMissing;
    K70WifiIdentity live={};
    /* Query only for protected files, after AMSS/QMI can report the identity. */
    const char *base=name;if(name)for(const char *p=name;*p;++p)if(*p=='/')base=p+1;
    bool protectedFile=false,calibration=false;
    if(base)for(size_t i=0;i<r->selection.count;++i)
        if(!strcmp(base,r->selection.files[i].request))
        {calibration=r->selection.files[i].role==K70WifiCalibration;
            protectedFile=calibration || r->selection.files[i].role==K70WifiRegdb;}
    if(protectedFile && r->backend.firmware_identity(r->backend.cookie,&live)) {
        if(data)*data=nullptr;if(bytes)*bytes=0;
        r->firmware_error=K70WifiUnqualified;return K70WifiUnqualified;
    }
    int rc=k70_wifi_ram_get(&r->selection,protectedFile?&live:nullptr,name,data,bytes);
    if(rc!=K70WifiMissing && rc)r->firmware_error=rc;
    if(!rc && calibration)r->calibration_served=1;
    return rc;
}
bool backendValid(const K70WifiBackend &b) {
    const auto &p=b.platform;
    return p.now_ms&&p.sleep_ms&&p.poll&&p.log&&p.halt&&p.alloc&&p.free&&
        b.power_and_pcie&&b.claim_dma&&b.attach_msi&&b.firmware_identity&&
        b.wake_dispatcher&&b.publish_network;
}
bool boardValid(const nw_board &b) {
    return b.device_id==0x1107&&b.bar&&b.bar_len>=4096&&b.cfg_read&&b.cfg_write&&
        b.map_phys&&b.msi_target&&b.msi_cap>=0x40&&b.msi_cap<=0xec&&
        !(b.msi_cap&3)&&b.irq_base>=0;
}
bool validMac(const uint8_t *mac) {
    uint8_t v=0;for(unsigned i=0;i<6;++i)v|=mac[i];return v && !(mac[0]&1);
}
}
int k70_wifi_qualify_selection(const K70WifiSelection *s) {
    if(!s||!s->count||s->count>K70WifiFirmwareMax)return K70WifiInvalid;
    if(!identityValid(s->identity)||memcmp(s->stock_module_sha256,stockModule,32)||
       !digestPresent(s->selection_evidence_sha256)||!digestPresent(s->driver_format_evidence_sha256))return K70WifiUnqualified;
    uint32_t roles=0;
    for(size_t i=0;i<s->count;++i) {
        const auto &f=s->files[i];
        if(!nameValid(f.request)||!nameValid(f.source)||!f.data||!f.bytes||
           f.role<K70WifiAmss||f.role>K70WifiMac||!digestPresent(f.sha256))return K70WifiInvalid;
        if(roles&(1u<<f.role))return K70WifiInvalid;
        roles|=1u<<f.role;
        for(size_t j=0;j<i;++j)if(!strcmp(f.request,s->files[j].request))return K70WifiInvalid;
        if(!fileHash(f))return K70WifiIntegrity;
    }
    return (roles&0x1e)==0x1e?0:K70WifiMissing;
}
int k70_wifi_ram_get(const K70WifiSelection *s,const K70WifiIdentity *live,
    const char *request,const uint8_t **data,size_t *bytes) {
    if(data)*data=nullptr;if(bytes)*bytes=0;
    if(!s||!request||!data||!bytes)return K70WifiInvalid;
    /* Linux supplies directories. Reject empty/dot components and traversal
     * before taking a basename; requests can never escape the RAM file table. */
    const char *base=request;
    for(const char *p=request;*p;++p) {
        if(*p=='\\')return K70WifiInvalid;
        if(*p=='/') {
            if(p==base||(p-base==1&&base[0]=='.')||
               (p-base==2&&base[0]=='.'&&base[1]=='.'))return K70WifiInvalid;
            base=p+1;
        }
    }
    if(!nameValid(base))return K70WifiInvalid;
    int rc=k70_wifi_qualify_selection(s);if(rc)return rc;
    for(size_t i=0;i<s->count;++i) {
        const auto &f=s->files[i];if(strcmp(base,f.request))continue;
        if((f.role==K70WifiCalibration||f.role==K70WifiRegdb)&&
           (!live||!sameIdentity(*live,s->identity)))return K70WifiUnqualified;
        *data=f.data;*bytes=f.bytes;return 0;
    }
    return K70WifiMissing;
}
int k70_wifi_startup(K70WifiRuntime *r,const K70WifiBackend *b,const K70WifiSelection *s) {
    if(!r||!b||!s)return K70WifiInvalid;
    int rc=k70_wifi_qualify_selection(s);if(rc)return rc;
    if(!backendValid(*b))return K70WifiUnqualified;
    uint32_t expected=K70WifiCold;
    if(!__atomic_compare_exchange_n(&r->state,&expected,uint32_t(K70WifiStarting),false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return K70WifiBusy;
    r->selection=*s;r->backend=*b;r->platform=b->platform;
    K70WifiRuntime *none=nullptr;
    if(!__atomic_compare_exchange_n(&installed,&none,r,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&r->state,uint32_t(K70WifiCold),__ATOMIC_RELEASE);return K70WifiBusy;
    }
    rc=b->power_and_pcie(b->cookie,&r->board);if(rc)return fail(r,rc);
    if(!boardValid(r->board))return fail(r,K70WifiInvalid);
    uint32_t endpoint=0;
    rc=r->board.cfg_read(0,4,&endpoint);if(rc)return fail(r,rc);
    if(endpoint!=0x110717cb)return fail(r,K70WifiWrongDevice);
    rc=b->claim_dma(b->cookie,0x1401,&r->dma);if(rc)return fail(r,rc);
    if(!r->dma.cpu||!r->dma.bus||!r->dma.bytes||!r->dma.sync_owned||r->dma.quarantined||
       (r->dma.page!=4096&&r->dma.page!=16384)||r->dma.count!=1||r->dma.used||
       (((uintptr_t)r->dma.cpu|r->dma.bus|r->dma.bytes)&(r->dma.page-1))||
       r->dma.bytes>UINTPTR_MAX-(uintptr_t)r->dma.cpu||r->dma.bytes>UINT64_MAX-r->dma.bus||
       r->dma.blocks[0].offset||r->dma.blocks[0].requested||r->dma.blocks[0].owned!=r->dma.bytes)
        return fail(r,K70WifiUnqualified);
    uint32_t lo=0,hi=0,target=0;
    rc=r->board.msi_target(&lo,&hi,&target);if(rc)return fail(r,rc);
    if(!(lo|hi))return fail(r,K70WifiUnqualified);
    rc=b->attach_msi(b->cookie,&r->board);if(rc)return fail(r,rc);
    r->platform.dma_alloc=dmaAlloc;r->platform.dma_free=dmaFree;
    r->platform.dma_map=dmaMap;r->platform.dma_sync=dmaSync;r->platform.fw_get=fwGet;
    rc=nw_wifi_start(&r->platform,&r->board);if(rc)return fail(r,rc);
    if(r->firmware_error)return fail(r,r->firmware_error);
    __atomic_store_n(&r->state,uint32_t(K70WifiStarted),__ATOMIC_RELEASE);return 0;
}
int k70_wifi_dispatch(K70WifiRuntime *r) {
    if(!r||current()!=r||__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=K70WifiStarted)return K70WifiUnqualified;
    if(__atomic_exchange_n(&r->dispatch_busy,1u,__ATOMIC_ACQ_REL))return K70WifiBusy;
    int rc=nw_wifi_step();if(rc<0)rc=fail(r,rc);
    if(r->firmware_error)rc=fail(r,r->firmware_error);
    if(rc>=0 && !r->network_published && r->calibration_served && nw_wifi_stats.stage>=4) {
        nw_wifi_status st={};nw_wifi_status_get(&st);
        if(st.channels && st.state!=NW_STA_OFF && validMac(st.mac)) {
            int result=r->backend.publish_network(r->backend.cookie,st.mac);
            if(result)rc=fail(r,result);else r->network_published=1;
        }
    }
    __atomic_store_n(&r->dispatch_busy,0u,__ATOMIC_RELEASE);return rc;
}
void sheng_wifi_wake(void) {
    auto *r=current();if(r && __atomic_load_n(&r->state,__ATOMIC_ACQUIRE)==K70WifiStarted)
        r->backend.wake_dispatcher(r->backend.cookie);
}
