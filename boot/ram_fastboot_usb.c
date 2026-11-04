/* K70 pre-XNU Fastboot RAM receiver. DWC3 sequence follows vermeer_usb_debug.c.
 * Requires live RAM ownership, ABL clocks and the existing identity SMMU route.
 * Fixed DMA replaces the XNU allocator; no clocks, SMMU or storage are changed.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "console.h"
#include "ram_guard.h"
#include "ram_fastboot.h"
#include "ram_fastboot_config.h"
#define VERMEER_HOST_TEST
#include "vermeer_usb_protocol.h"

#define DWC UINT64_C(0xa600000)
#define GCC UINT64_C(0x139000)
#define SMMU UINT64_C(0x15000000)
#define WRAPPER UINT64_C(0xa6f8800)
#define DMA UINT64_C(0x8c0000000)
#define DMA_BYTES 0x20000u
#define ROOT UINT64_C(0x940000000)
#define GCTL 0xc110
#define DCFG 0xc700
#define DCTL 0xc704
#define DSTS 0xc70c
#define DEVTEN 0xc708
#define DALEPENA 0xc720
#define EVLO 0xc400
#define EVHI 0xc404
#define EVSIZE 0xc408
#define EVCOUNT 0xc40c
#define RUN (1u<<31)
#define RESET (1u<<30)
#define HALTED (1u<<22)
#define EP(n) (0xc800+16*(n))
#define ACTIVE (1u<<10)
#define SET_CONFIG 1u
#define SET_RESOURCE 2u
#define STALL 4u
#define CLEAR_STALL 5u
#define START 6u
#define END 8u
#define START_CONFIG 9u
#define FORCE_REMOVE (1u<<11)
#define NORMAL (1u<<4)
#define SETUP_TRB (2u<<4)
#define STATUS2 (3u<<4)
#define STATUS3 (4u<<4)
#define DATA_TRB (5u<<4)
#define SETUP 0x1200u
#define CONTROL 0x1300u
#define TX 0x2000u
#define RX 0x8000u
#define RX_BYTES 0x10000u
#define EVENT_BYTES 4096u
static struct {
    bool failed,touched,configured,busy[4],data_in,reply_pending,commit_info,session_started;
    unsigned mps,pos,requested[4],resource[4],enumeration_resets;
    enum { SETUP_PHASE,DATA_PHASE,WAIT2,WAIT3,STATUS_PHASE } phase;
    const char *reason;
    char reply[65];
    uint32_t power,master,cfg,aggre,id,hw,gctl,id0,id1,id2,ctl,route,cbar,sctlr;
    unsigned observed;
} u;
static struct rfb_state receiver;
static const unsigned trbs[4]={0x1000,0x1040,0x1080,0x10c0};
static const uint8_t device[18]={18,1,0,2,0,0,0,64,0xd1,0x18,0x0d,0xd0,0,1,1,2,3,1};
static const uint8_t config[32]={9,2,32,0,1,1,0,0x80,250,9,4,0,0,2,0xff,0x42,3,0,
    7,5,1,2,0,2,0,7,5,0x81,2,0,2,0};
static const uint8_t qualifier[10]={10,6,0,2,0,0,0,64,1,0};
static uint64_t now(void) {uint64_t v;__asm__ volatile("mrs %0,cntpct_el0":"=r"(v));return v;}
static uint64_t frequency(void) {uint64_t v;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(v));return v;}
static void barrier(void) {__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(uintptr_t base,unsigned off)
{uint32_t v=*(volatile uint32_t *)(base+off);__asm__ volatile("dmb oshld":::"memory");return v;}
static void wr(unsigned off,uint32_t v) {*(volatile uint32_t *)(DWC+off)=v;barrier();}
static void wait_us(unsigned us)
{uint64_t begin=now(),ticks=(frequency()*us+999999)/1000000;while(now()-begin<ticks)__asm__ volatile("yield");}
static void copy(void *dst,const void *src,unsigned n)
{uint8_t *d=dst;const uint8_t *s=src;for(unsigned i=0;i<n;i++)d[i]=s[i];}
static void fail(const char *why) {if(!u.failed)u.reason=why;u.failed=true;}
static int command(unsigned ep,unsigned cmd,uint32_t p0,uint32_t p1,uint32_t p2)
{
    if(u.failed || ep>=4)return -1;
    wr(EP(ep)+8,p0);wr(EP(ep)+4,p1);wr(EP(ep),p2);wr(EP(ep)+12,cmd|ACTIVE);
    for(unsigned i=0;i<1000;i++) {
        uint32_t v=rd(DWC,EP(ep)+12);
        if(!(v&ACTIVE)) {
            if((v>>12)&15) {con_hex("EP",ep);con_hex("CMD",cmd);con_hex("STATUS",v);fail("endpoint command status");return -1;}
            if((cmd&15)==START)u.resource[ep]=(v>>16)&0x7f;
            return 0;
        }
        wait_us(1);
    }
    fail("endpoint command timeout");return -1;
}
static void transfer(unsigned ep,unsigned offset,unsigned bytes,unsigned kind)
{
    if(u.failed)return;
    if(ep>=4 || u.busy[ep] || offset>DMA_BYTES || bytes>DMA_BYTES-offset) {fail("DMA transfer bounds/ownership");return;}
    uint64_t address=DMA+offset,trb=DMA+trbs[ep];
    volatile uint32_t *t=(volatile uint32_t *)(uintptr_t)trb;
    t[3]=0;t[0]=(uint32_t)address;t[1]=(uint32_t)(address>>32);t[2]=bytes;
    /* Publish the descriptor before giving hardware ownership (HWO).
     * Like raphael patch 0122, descriptors need ordered, uncached access.
     * This shim already requires MMU and data cache off. */
    barrier();t[3]=kind|1u|2u|(1u<<10)|(1u<<11);
    u.requested[ep]=bytes;u.busy[ep]=true;barrier();
    (void)command(ep,START,(uint32_t)(trb>>32),(uint32_t)trb,0);
}
static int complete(unsigned ep)
{
    if(ep>=4 || !u.busy[ep]) {fail("unexpected DMA completion");return -1;}
    barrier();volatile uint32_t *t=(volatile uint32_t *)(uintptr_t)(DMA+trbs[ep]);
    unsigned left=t[2]&0xffffff;
    if((t[3]&1) || (t[2]>>28) || left>u.requested[ep] || ((ep&1) && left)) {fail("DMA completion status/bounds");return -1;}
    u.busy[ep]=false;return (int)(u.requested[ep]-left);
}
static void setup_arm(void) {u.phase=SETUP_PHASE;transfer(0,SETUP,8,SETUP_TRB);}
static void stall(void) {(void)command(0,STALL,0,0,0);setup_arm();}
static void receive_arm(void)
{
    if(receiver.complete || receiver.failed || u.failed)return;
    unsigned n=receiver.remaining;
    if(!n)n=u.mps;else {if(n>RX_BYTES)n=RX_BYTES;n=(n+u.mps-1)&~(u.mps-1);}
    transfer(2,RX,n,NORMAL);
}
static unsigned length(const char *s) {unsigned n=0;while(s[n])n++;return n;}
static void reply_send(void)
{
    if(!u.reply_pending || u.busy[3] || u.failed)return;
    unsigned n=length(u.reply);
    if(!n || n>64) {fail("Fastboot reply bounds");return;}
    copy((void *)(uintptr_t)(DMA+TX),u.reply,n);u.reply_pending=false;transfer(3,TX,n,NORMAL);
}
static void configure(void)
{
    if(u.configured || u.failed)return;
    if(command(0,START_CONFIG|(2u<<16),0,0,0) ||
       command(2,SET_CONFIG,(2u<<1)|(u.mps<<3),(1u<<8)|(2u<<25),0) || command(2,SET_RESOURCE,1,0,0) ||
       command(3,SET_CONFIG,(2u<<1)|(u.mps<<3)|(1u<<17),(1u<<8)|(3u<<25),0) || command(3,SET_RESOURCE,1,0,0))return;
    wr(DALEPENA,15);u.configured=true;receive_arm();con_str("USB","Fastboot interface configured");
}
static unsigned utf16(uint8_t *dst,const char *s,bool terminator)
{unsigned n=0;while(*s) {dst[n++]=(uint8_t)*s++;dst[n++]=0;}if(terminator) {dst[n++]=0;dst[n++]=0;}return n;}
static void le16(uint8_t *p,unsigned v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void le32(uint8_t *p,unsigned v) {le16(p,v);le16(p+2,v>>16);}
static unsigned string(unsigned index,uint8_t *dst)
{
    if(!index) {dst[0]=4;dst[1]=3;dst[2]=9;dst[3]=4;return 4;}
    const char *s=index==1?"Vermeer Lab":index==2?"K70 RAM Fastboot":index==3?"VERMEERRAM001":index==0xee?"MSFT100":NULL;
    if(!s)return 0;
    unsigned n=2+utf16(dst+2,s,false);
    if(index==0xee) {dst[n++]=0x20;dst[n++]=0;}
    dst[0]=(uint8_t)n;dst[1]=3;return n;
}
static unsigned windows_descriptor(unsigned index,uint8_t *p)
{
    for(unsigned i=0;i<256;i++)p[i]=0;
    if(index==4) {
        le32(p,40);le16(p+4,0x100);le16(p+6,4);p[8]=1;p[17]=1;
        copy(p+18,"WINUSB",6);return 40;
    }
    if(index==5) {
        unsigned name=20; /* 10-byte header, dwSize/dwType, wNameLength. */
        unsigned name_bytes=utf16(p+name,"DeviceInterfaceGUIDs",true);
        unsigned data_pos=name+name_bytes+4;
        unsigned data_bytes=utf16(p+data_pos,"{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}",true)+2;
        unsigned total=data_pos+data_bytes;
        le32(p,total);le16(p+4,0x100);le16(p+6,5);le16(p+8,1);
        le32(p+10,total-10);le32(p+14,7);le16(p+18,name_bytes);le32(p+name+name_bytes,data_bytes);
        return total;
    }
    return 0;
}
static void setup(void)
{
    const uint8_t *s=(const uint8_t *)(uintptr_t)(DMA+SETUP);uint8_t *p=(uint8_t *)(uintptr_t)(DMA+CONTROL);
    unsigned value=s[2]|(s[3]<<8),index=s[4]|(s[5]<<8),bytes=s[6]|(s[7]<<8),n=0;
    u.data_in=(s[0]&0x80)!=0;
    if((s[0]==0xc0 || s[0]==0xc1) && s[1]==0x20 && !value) {
        n=windows_descriptor(index,p);if(!n) {stall();return;}
    } else if(s[0]==0x80 && s[1]==6) {
        switch(value>>8) {
        case 1:if(!(value&255)) {copy(p,device,sizeof(device));n=sizeof(device);}break;
        case 2:case 7:
            if(value&255)break;copy(p,config,sizeof(config));n=sizeof(config);p[1]=(uint8_t)(value>>8);
            {unsigned m=(value>>8)==7?(u.mps==512?64:512):u.mps;le16(p+22,m);le16(p+29,m);}break;
        case 3:n=string(value&255,p);break;
        case 6:if(!(value&255)) {copy(p,qualifier,sizeof(qualifier));n=sizeof(qualifier);}break;
        }
        if(!n) {stall();return;}
    } else if(!s[0] && s[1]==5 && value<128 && !index && !bytes)wr(DCFG,(rd(DWC,DCFG)&~(127u<<3))|(value<<3));
    else if(!s[0] && s[1]==9 && value<=1 && !index && !bytes) {
        if(!value && u.configured) {fail("host deconfigured live session");return;}
        if(value)configure();
    } else if(s[0]==0x80 && s[1]==8 && !value && !index && bytes==1) {p[0]=u.configured;n=1;}
    else if((s[0]==0x80 || s[0]==0x81 || s[0]==0x82) && !s[1] && !value && bytes==2) {p[0]=p[1]=0;n=2;}
    else if(s[0]==1 && s[1]==11 && !index && !value && !bytes) {}
    else if(s[0]==0x81 && s[1]==10 && !index && !value && bytes==1) {p[0]=0;n=1;}
    else if(s[0]==2 && s[1]==1 && !value && !bytes && (index==1 || index==0x81)) {
        unsigned ep=index==1?2:3;if(u.busy[ep]) {fail("clear halt while DMA owned");return;}(void)command(ep,CLEAR_STALL,0,0,0);
    } else {stall();return;}
    if(u.failed)return;
    if(!bytes) {u.phase=WAIT2;return;}
    if(!u.data_in || !n) {stall();return;}
    u.phase=DATA_PHASE;transfer(1,CONTROL,n<bytes?n:bytes,DATA_TRB);
}
static void reset_control(void)
{
    if(u.phase==SETUP_PHASE)return;
    for(unsigned ep=0;ep<2;ep++)if(u.busy[ep]) {
        if(command(ep,END|FORCE_REMOVE|(u.resource[ep]<<16),0,0,0))return;
        u.busy[ep]=false;u.resource[ep]=0;
    }
    u.data_in=false;stall();
}
static void reset_enumeration(const char *why)
{
    /* Raphael 0127 resets endpoints so the host can configure again.
     * RAM Fastboot permits this only BEFORE the first protocol packet.
     * Never retry/resume an interrupted root or forget its hash state. */
    if(u.session_started || receiver.received || receiver.remaining ||
       receiver.complete || receiver.failed || u.reply_pending || u.commit_info) {
        fail("USB reset/disconnect after Fastboot started");return;
    }
    if(u.enumeration_resets>=8) {fail("USB enumeration reset limit");return;}
    u.enumeration_resets++;
    if(!u.configured) {reset_control();return;}
    for(unsigned ep=0;ep<4;ep++)if(u.busy[ep]) {
        if(command(ep,END|FORCE_REMOVE|(u.resource[ep]<<16),0,0,0))return;
        u.busy[ep]=false;u.resource[ep]=0;
    }
    wr(DALEPENA,3);u.configured=false;u.data_in=false;u.reply[0]=0;
    setup_arm();con_str("USB",why);
}
static void event(uint32_t e)
{
    if(e&1) {
        if(e&0xfe) {fail("device event format");return;}
        unsigned type=(e>>8)&15;
        if(type==1) {
            reset_enumeration("enumeration reset; waiting for host configuration");
            if(!u.failed)wr(DCFG,rd(DWC,DCFG)&~(127u<<3));
        } else if(type==2) {
            unsigned speed=rd(DWC,DSTS)&7;
            if(speed!=0 && speed!=1 && speed!=3) {fail("USB2 speed guard");return;}
            u.mps=speed==0?512:64;
            (void)command(0,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10),0);
            (void)command(1,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10)|(1u<<25),0);
        } else if(!type)reset_enumeration("enumeration disconnect; waiting for host configuration");
        else if(type==9 || type==11)fail("controller event overflow");
        return;
    }
    unsigned ep=(e>>1)&31,type=(e>>6)&15,status=(e>>12)&15;
    if(type==1) {
        int n=complete(ep);if(n<0)return;
        if(status&1) {fail("USB transfer bus error");return;}
        if(ep==2) {
            u.session_started=true;
            int result=receiver.remaining?rfb_data(&receiver,(const void *)(uintptr_t)(DMA+RX),(unsigned)n,u.reply):
                rfb_command(&receiver,(const uint8_t *)(uintptr_t)(DMA+RX),(unsigned)n,u.reply);
            /* fastboot CLI does not print the payload of an OEM OKAY. Emit
             * INFO first so the host can record the explicit SHA result. */
            if(result==1) {copy(u.reply,"INFOroot SHA256 verified",25);u.commit_info=true;}
            u.reply_pending=u.reply[0]!=0;
            if(u.reply_pending)reply_send();else receive_arm();
            return;
        }
        if(ep==3) {
            if(receiver.failed)fail("Fastboot rejected data/command; see host log");
            else if(u.commit_info) {
                u.commit_info=false;copy(u.reply,"OKAYroot SHA256 verified",25);u.reply_pending=true;reply_send();
            } else receive_arm();
            return;
        }
        switch(u.phase) {
        case SETUP_PHASE:if(ep!=0 || n!=8)fail("setup length");else setup();break;
        case DATA_PHASE:if(ep!=1)fail("control data direction");else u.phase=WAIT3;break;
        case STATUS_PHASE:if(n)fail("control status length");else setup_arm();break;
        default:fail("control completion order");break;
        }
    } else if(ep<=1 && type==3 && status==2 && (u.phase==WAIT2 || u.phase==WAIT3)) {
        unsigned direction=u.phase==WAIT2?1:(u.data_in?0:1);
        if(ep!=direction) {fail("control status direction");return;}
        unsigned kind=u.phase==WAIT3?STATUS3:STATUS2;u.phase=STATUS_PHASE;transfer(ep,CONTROL,0,kind);
    }
}
static void poll(void)
{
    unsigned count=rd(DWC,EVCOUNT)&0xffff;
    if(count>EVENT_BYTES || (count&3)) {fail("event ring bounds");return;}
    barrier();
    for(unsigned i=0;i<count && !u.failed;i+=4) {
        uint32_t e=*(volatile uint32_t *)(uintptr_t)(DMA+u.pos);u.pos=(u.pos+4)%EVENT_BYTES;event(e);
    }
    if(count)wr(EVCOUNT,count);
}
static bool identity(void)
{
    u.id0=rd(SMMU,0x20);u.id1=rd(SMMU,0x24);u.id2=rd(SMMU,0x28);u.ctl=rd(SMMU,0);u.observed=3;
    if(!vusb_smmu_layout(u.id0,u.id1,u.id2,u.ctl)) {fail("SMMU layout/control refused");return false;}
    unsigned matches=0;
    for(unsigned i=0;i<105;i++)if(vusb_stream_match(rd(SMMU,0x800+4*i))) {matches++;u.route=rd(SMMU,0xc00+4*i);}
    if(!matches) {fail("SID40 route missing; DMA cannot reach RAM");return false;}
    if(matches!=1) {fail("SID40 route is ambiguous");return false;}
    u.sctlr=1;
    if(((u.route>>16)&3)==0) {
        unsigned cb=u.route&255;if(cb>=70) {fail("SMMU context range");return false;}
        u.cbar=rd(SMMU,0x1000+4*cb);u.sctlr=rd(SMMU,0x80000+0x1000*cb);
    }
    if(!vusb_route_identity(u.route,u.cbar,u.sctlr)) {fail("SID40 is not identity");return false;}
    return true;
}
static bool reset_device(void)
{
    wr(DCTL,RESET);
    for(unsigned i=0;i<500 && (rd(DWC,DCTL)&RESET);i++)wait_us(1000);
    if(rd(DWC,DCTL)&RESET) {fail("DWC3 reset timeout");return false;}
    /* Linux dwc3_core_soft_reset and raphael 0114: on older DWC_usb31,
     * CSFTRST clears before clock synchronization finishes (>=50 ms).
     * Apply the bounded delay conservatively to the whole detected 3.1
     * family; do not infer the K70 revision from the K20 Pro's revision. */
    if((u.id>>16)==0x3331)wait_us(50000);
    return true;
}
static void start(void)
{
    u.mps=512;u.power=rd(GCC,4);u.master=rd(GCC,0x18);u.cfg=rd(GCC,0x88);u.aggre=rd(GCC,0x8c);u.observed=1;
    if(!(u.power&(1u<<31)) || ((u.master|u.cfg|u.aggre)&(1u<<31))) {fail("ABL USB clock/power gate");return;}
    u.id=rd(DWC,0xc120);u.hw=rd(DWC,0xc140);u.gctl=rd(DWC,GCTL);u.observed=2;
    if((u.id>>16)!=0x5533 && (u.id>>16)!=0x3331) {fail("DWC3 identity");return;}
    if(((u.gctl>>12)&3)!=2) {fail("DWC3 not device mode");return;}
    if(!identity())return;
    if(!vusb_dma_allowed(DMA,DMA_BYTES,u.hw>>24)) {fail("DMA address width");return;}
    u.touched=true;wr(DEVTEN,0);wr(DCTL,rd(DWC,DCTL)&~RUN);
    unsigned i;for(i=0;i<500 && !(rd(DWC,DSTS)&HALTED);i++)wait_us(1000);
    if(!(rd(DWC,DSTS)&HALTED)) {fail("DWC3 halt timeout");return;}
    if(!reset_device())return;
    for(unsigned off=0;off<DMA_BYTES;off+=8)*(volatile uint64_t *)(uintptr_t)(DMA+off)=0;
    barrier();wr(EVLO,(uint32_t)DMA);wr(EVHI,(uint32_t)(DMA>>32));
    if(rd(DWC,EVLO)!=(uint32_t)DMA || rd(DWC,EVHI)!=(uint32_t)(DMA>>32)) {fail("DMA address readback");return;}
    wr(EVSIZE,(1u<<31)|EVENT_BYTES);wr(EVCOUNT,rd(DWC,EVCOUNT)&0xffff);wr(DCFG,rd(DWC,DCFG)&~(7u|(127u<<3)));
    if(command(0,START_CONFIG,0,0,0) || command(0,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10),0) ||
       command(0,SET_RESOURCE,1,0,0) || command(1,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10)|(1u<<25),0) || command(1,SET_RESOURCE,1,0,0))return;
    wr(DALEPENA,3);setup_arm();if(u.failed)return;
    uint32_t hs=rd(WRAPPER,0x10),ss=rd(WRAPPER,0x30);
    *(volatile uint32_t *)(WRAPPER+0x10)=hs|(1u<<20)|(1u<<28);
    *(volatile uint32_t *)(WRAPPER+0x30)=ss|(1u<<24);barrier();
    wr(DEVTEN,7|(1u<<9)|(1u<<11));wr(DCTL,rd(DWC,DCTL)|RUN);
    for(i=0;i<500 && (rd(DWC,DSTS)&HALTED);i++)wait_us(1000);
    if(rd(DWC,DSTS)&HALTED)fail("DWC3 run timeout");
}
static bool halt(void)
{
    if(!u.touched)return true;
    wr(DEVTEN,0);wr(DCTL,rd(DWC,DCTL)&~RUN);
    for(unsigned i=0;i<500 && !(rd(DWC,DSTS)&HALTED);i++)wait_us(1000);
    if(!(rd(DWC,DSTS)&HALTED))return false;
    wr(DALEPENA,0);wr(EVSIZE,1u<<31);wr(EVLO,0);wr(EVHI,0);barrier();return true;
}
static bool handoff_ready(void)
{return receiver.complete && !u.failed && !u.busy[3] && !u.reply_pending && !u.commit_info;}
static int write_root(void *ctx,uint64_t offset,const void *data,unsigned n)
{
    (void)ctx;if(offset>RFB_ROOT_BYTES || n>RFB_ROOT_BYTES-offset)return -1;
    const uint8_t *s=data;volatile uint8_t *d=(volatile uint8_t *)(uintptr_t)(ROOT+offset);
    unsigned i=0;
    if(!((offset|(uintptr_t)data)&7))for(;i+8<=n;i+=8)*(volatile uint64_t *)(d+i)=*(const uint64_t *)(s+i);
    for(;i<n;i++)d[i]=s[i];return 0;
}
int ram_fastboot_receive(uintptr_t dtb);
int ram_fastboot_receive(uintptr_t dtb)
{
    uint64_t sctlr;__asm__ volatile("mrs %0,sctlr_el1":"=r"(sctlr));
    if(sctlr&5) {con_str("REFUSED","MMU/data cache must be off");return -1;}
    if(!frequency() || frequency()>UINT64_C(1000000000)) {con_str("REFUSED","counter frequency");return -1;}
    extern char _head[],_end[];
    if(dtb<0xb5d00000 || dtb>=0xb5f00000 || (dtb&7) ||
       ram_guard((const uint8_t *)dtb,0xb5f00000-dtb,HIGH_RAM_LO,UINT64_C(0xac0000000)) ||
       ((uintptr_t)_head<UINT64_C(0xac0000000) && (uintptr_t)_end>HIGH_RAM_LO)) {
        con_str("REFUSED","live RAM ownership/overlap");return -1;
    }
    volatile struct rfb_ops ops;ops.ctx=NULL;ops.write=write_root;
    ops.log_size=con_log_size;ops.log_read=con_log_read;
    rfb_init(&receiver,(const struct rfb_ops *)&ops,RFB_ROOT_BYTES,rfb_root_sha);
    if(receiver.failed)return -1;
    con_str("RAM","live lower 8 GiB guard passed");start();
    uint64_t progress=0,deadline=now()+frequency()*300,last_screen=now();
    if(!u.failed) {con_str("USB","waiting for VERMEERRAM001 Fastboot");con_hex("ROOTSIZE",RFB_ROOT_BYTES);}
    while(!u.failed && !handoff_ready()) {
        poll();
        if(receiver.received!=progress) {progress=receiver.received;deadline=now()+frequency()*300;}
        if(now()>deadline) {fail("Fastboot idle timeout");break;}
        if(now()-last_screen>frequency()*15) {
            con_clear(BLUE);con_str("VERMEER","RAM Fastboot before XNU");con_hex("RECEIVED",progress);con_hex("TOTAL",RFB_ROOT_BYTES);
            con_str("STATE",u.configured?"receiving or waiting for next chunk":"waiting for computer Fastboot");last_screen=now();
        }
        __asm__ volatile("yield");
    }
    if(!halt()) {fail("USB DMA halt failed; XNU not started");}
    if(u.failed || !receiver.complete) {
        con_clear(BLUE);con_str("STOPPED",u.reason?u.reason:"root not complete");con_hex("RECEIVED",receiver.received);
        if(u.observed>=1) {con_hex("POWER",u.power);con_hex("MASTER",u.master);con_hex("CFG",u.cfg);con_hex("AGGRE",u.aggre);}
        if(u.observed>=2) {con_hex("DWCID",u.id);con_hex("HWPARAM",u.hw);con_hex("GCTL",u.gctl);}
        if(u.observed>=3) {con_hex("SMMUID0",u.id0);con_hex("SMMUID1",u.id1);con_hex("SMMUID2",u.id2);con_hex("SMMUCTL",u.ctl);con_hex("ROUTE",u.route);con_hex("CBAR",u.cbar);con_hex("SCTLR",u.sctlr);}
        return -1;
    }
    barrier();con_str("ROOT","full packed SHA256 passed");con_str("USB","DMA halted; handing root to XNU");return 0;
}
