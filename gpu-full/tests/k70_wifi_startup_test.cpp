/* SPDX-License-Identifier: MIT */
#include "K70WifiStartup.h"
#include "NativeAdrenoSha256.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static K70WifiRuntime runtime,other;
static K70WifiSelection selection;
static K70WifiBackend backend;
static const lx_platform *driverPlatform;
static const char *scenario;
static char order[32];static unsigned orderLength,wakes,published,driverCalls,steps;
static unsigned pageBytes;
static uint8_t dmaStorage[65536+16383];
static uint8_t *dmaPages;
static const size_t arenaBytes=65536;
static uint8_t fileBytes[4][3]={{'a','b','c'},{'a','b','c'},{'a','b','c'},{'a','b','c'}};
static K70WifiIdentity live;
static uint64_t allocationBus;static void *allocation;
static bool is(const char *s){return !strcmp(scenario,s);}
static void phase(char c){order[orderLength++]=c;order[orderLength]=0;}
static uint64_t now(){return 10;}
static void sleep(unsigned){}
static void poll(){}
static void log(int,const char*){}
static void halt(){std::abort();}
static int sync(void*,void*,size_t,int){return is("dma_cache_failure")?-1:0;}
static int cfgRead(int off,int size,uint32_t *value){assert(off==0&&size==4);*value=is("wrong_endpoint")?0x110317cb:0x110717cb;return 0;}
static int cfgWrite(int,int,uint32_t){return 0;}
static int msiTarget(uint32_t *lo,uint32_t *hi,uint32_t *data){*lo=0x400000;*hi=0;*data=1;return 0;}
static void *mapPhys(uint64_t,size_t){return dmaPages;}
static int power(void*,nw_board *b){phase('P');if(is("power_failure"))return -5;
    b->bar=dmaPages;b->bar_len=arenaBytes;b->device_id=0x1107;b->msi_cap=0x80;
    b->irq_base=1000;b->cfg_read=cfgRead;b->cfg_write=cfgWrite;b->map_phys=mapPhys;b->msi_target=msiTarget;return 0;}
static int claimDma(void*,uint32_t sid,K70WifiDmaArena *a){phase('D');assert(sid==0x1401);
    if(is("dma_failure"))return -7;
    int rc=k70_wifi_dma_init(a,dmaPages,0x800000,arenaBytes,pageBytes,nullptr,sync);
    if(is("bad_dma_alignment"))++a->bus;
    return rc;}
static int attachMsi(void*,const nw_board*){phase('M');return is("msi_failure")?-9:0;}
static int identity(void*,K70WifiIdentity *id){*id=live;return is("identity_unavailable")?-2:0;}
static void wake(void*){++wakes;}
static int publish(void*,const uint8_t mac[6]){assert(mac[0]==2);++published;return 0;}
extern "C" {
struct nw_wifi_stats nw_wifi_stats;
int nw_wifi_start(const lx_platform *p,const nw_board*) {
    phase('S');++driverCalls;driverPlatform=p;
    const uint8_t *d=nullptr;size_t n=0;
    assert(p->fw_get("ath12k/WCN7850/hw2.0/amss.bin",&d,&n)==0&&n==3&&d[0]=='a');
    allocation=p->dma_alloc(4097,&allocationBus);
    if(is("dma_cache_failure")){assert(!allocation&&!allocationBus);return -13;}
    assert(allocation&&allocationBus%8192==0);
    assert(p->dma_map(allocation,4097,1)==allocationBus);
    p->dma_sync(allocationBus,4097,1);
    if(is("driver_failure"))return -11;
    p->dma_free(0,allocation,is("dma_free_failure")?allocationBus+1:0);
    if(!is("dma_free_failure"))allocation=nullptr;return 0;
}
int nw_wifi_step(void) {
    ++steps;if(is("dispatch_error"))return -14;
    if(is("concurrent_dispatch"))assert(k70_wifi_dispatch(&runtime)==K70WifiBusy);
    const uint8_t *d=nullptr;size_t n=0;
    if(is("corrupt_after_start"))fileBytes[0][0]='x';
    if(is("identity_mismatch"))++live.board_id;
    const char *request=is("regdb_only")?"regdb.bin":"board.bin";
    int rc=driverPlatform->fw_get(request,&d,&n);
    if(is("identity_mismatch")||is("identity_unavailable")||is("corrupt_after_start"))assert(rc<0&&!d&&!n);
    else assert(rc==0&&d&&n==3);
    nw_wifi_stats.stage=4;return 0;
}
void nw_wifi_status_get(nw_wifi_status *s){memset(s,0,sizeof *s);s->state=NW_STA_IDLE;s->channels=3;s->mac[0]=2;s->mac[5]=1;}
}
static void setup() {
    dmaPages=reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(dmaStorage)+16383)&~uintptr_t(16383));
    selection.identity={K70WifiIdentityAll,0x1234,2,0x77,0x17cb,0x1107,{'C','N'}};
    live=selection.identity;
    const uint8_t module[32]={0xe0,0x2d,0xff,0x73,0x76,0xec,0x96,0x67,0x14,0x54,0x20,0x9a,0x28,0x3a,0x1d,0xdd,
        0xd3,0x93,0x7f,0xdb,0xe6,0x8e,0x3f,0x25,0xea,0x37,0xbe,0x9b,0xa3,0x5d,0x35,0x28};
    memcpy(selection.stock_module_sha256,module,32);selection.selection_evidence_sha256[0]=1;
    selection.driver_format_evidence_sha256[0]=1;selection.count=4;
    const char *requests[]={"amss.bin","m3.bin","board.bin","regdb.bin"};
    const char *sources[]={"amss20.bin","phy_ucode20.elf","bd_n81a.elf","regdb.bin"};
    for(unsigned i=0;i<4;++i){auto &f=selection.files[i];strcpy(f.request,requests[i]);strcpy(f.source,sources[i]);
        f.role=i+1;f.data=fileBytes[i];f.bytes=3;NativeAdrenoSha256::hash(f.data,f.bytes,f.sha256);}
    backend.platform.now_ms=now;backend.platform.sleep_ms=sleep;backend.platform.poll=poll;
    backend.platform.log=log;backend.platform.halt=halt;backend.platform.alloc=malloc;backend.platform.free=free;
    backend.power_and_pcie=power;backend.claim_dma=claimDma;backend.attach_msi=attachMsi;
    backend.firmware_identity=identity;backend.wake_dispatcher=wake;backend.publish_network=publish;
}
static void preflight() {
    assert(k70_wifi_qualify_selection(&selection)==0);
    auto copy=selection;copy.identity.known=K70WifiIdentityEndpoint;
    assert(k70_wifi_qualify_selection(&copy)==K70WifiUnqualified);
    copy=selection;copy.stock_module_sha256[0]^=1;assert(k70_wifi_qualify_selection(&copy)==K70WifiUnqualified);
    copy=selection;memset(copy.driver_format_evidence_sha256,0,32);assert(k70_wifi_qualify_selection(&copy)==K70WifiUnqualified);
    copy=selection;copy.files[0].sha256[0]^=1;assert(k70_wifi_qualify_selection(&copy)==K70WifiIntegrity);
    copy=selection;copy.files[1].role=1;assert(k70_wifi_qualify_selection(&copy)==K70WifiInvalid);
    copy=selection;strcpy(copy.files[1].request,"amss.bin");assert(k70_wifi_qualify_selection(&copy)==K70WifiInvalid);
    copy=selection;copy.count=3;assert(k70_wifi_qualify_selection(&copy)==K70WifiMissing);
    copy=selection;copy.files[1]=copy.files[2];copy.files[2]=copy.files[3];copy.count=3;
    assert(k70_wifi_qualify_selection(&copy)==0); /* WCN7850 never requests m3.bin. */
    copy=selection;strcpy(copy.files[0].source,"../amss.bin");assert(k70_wifi_qualify_selection(&copy)==K70WifiInvalid);
    const uint8_t *d=(const uint8_t*)1;size_t n=9;
    assert(k70_wifi_ram_get(&selection,nullptr,"board.bin",&d,&n)==K70WifiUnqualified&&!d&&!n);
    auto wrong=live;++wrong.chip_revision;
    assert(k70_wifi_ram_get(&selection,&wrong,"board.bin",&d,&n)==K70WifiUnqualified&&!d&&!n);
    assert(k70_wifi_ram_get(&selection,&live,"ath12k/WCN7850/hw2.0/board.bin",&d,&n)==0&&n==3);
    const char *badNames[]={"../board.bin","ath12k/../board.bin","/board.bin","ath12k//board.bin","ath12k\\board.bin"};
    for(auto name:badNames)
        assert(k70_wifi_ram_get(&selection,&live,name,&d,&n)==K70WifiInvalid&&!d&&!n);
    assert(k70_wifi_ram_get(&selection,&live,"unlisted.bin",&d,&n)==K70WifiMissing&&!d&&!n);
    assert(!orderLength&&!driverCalls);
}
int main(int argc,char **argv) {
    assert(argc==3);scenario=argv[1];pageBytes=(unsigned)atoi(argv[2]);assert(pageBytes==4096||pageBytes==16384);setup();
    sheng_wifi_wake();assert(!wakes);
    if(is("preflight")){preflight();puts("preflight: 19 rejection and identity checks passed");return 0;}
    if(is("missing_backend")){backend.attach_msi=nullptr;assert(k70_wifi_startup(&runtime,&backend,&selection)==K70WifiUnqualified);assert(!orderLength);return 0;}
    int rc=k70_wifi_startup(&runtime,&backend,&selection);
    if(is("power_failure")||is("wrong_endpoint")||is("dma_failure")||is("msi_failure")||is("driver_failure")||
       is("dma_cache_failure")||is("dma_free_failure")||is("bad_dma_alignment")) {
        assert(rc<0&&runtime.state==K70WifiQuarantined&&runtime.dma.quarantined);
        assert(k70_wifi_startup(&runtime,&backend,&selection)==K70WifiBusy);
        sheng_wifi_wake();assert(!wakes);
        if(is("driver_failure")){assert(allocation&&runtime.dma.used>=4097);assert(!k70_wifi_dma_alloc(&runtime.dma,1,&allocationBus));}
        puts("post-power failure retained ownership and blocked restart");return 0;
    }
    assert(rc==0&&!strcmp(order,"PDMS")&&runtime.state==K70WifiStarted&&!runtime.network_published);
    sheng_wifi_wake();assert(wakes==1&&steps==0);
    assert(k70_wifi_startup(&runtime,&backend,&selection)==K70WifiBusy);
    assert(k70_wifi_startup(&other,&backend,&selection)==K70WifiBusy&&other.state==K70WifiCold);
    rc=k70_wifi_dispatch(&runtime);
    if(is("identity_mismatch")||is("identity_unavailable")||is("corrupt_after_start")||is("dispatch_error"))
        assert(rc<0&&runtime.state==K70WifiQuarantined&&runtime.dma.quarantined&&!published);
    else if(is("regdb_only"))assert(!published&&!runtime.calibration_served);
    else {assert(rc==0&&published==1&&runtime.calibration_served&&runtime.network_published);
        assert(k70_wifi_dispatch(&runtime)==0&&published==1);}
    printf("%s/%u passed (mock hardware only)\n",scenario,pageBytes);return 0;
}
