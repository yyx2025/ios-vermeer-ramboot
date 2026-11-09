/* K70 GT9916S touch: r33s SE DMA, RPMh rails and THP processing ported
 * to the iOS16 atomic HID collection API. Exact model/opt-in guards retained.
 * No GPI, flash writes, firmware upload or persistent storage access.
 * Reference: UniXNU-vermeer 7419b8b0; QcomGeniSpi BSD-2-Clause-Patent.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/thread_act.h>
#include <machine/machine_routines.h>
#include <machine/atomic.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "vermeer_touch_thp.h"
#include "vermeer_goodix_info.h"
#include "vermeer_touch_clock.h"
#include "SmmuBypass.h"
#include "qcom-geni-spi/GeniSpi.c"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#include "k70_rpmh.h"
extern vm_offset_t ml_vtophys(vm_offset_t);
extern int sweet_hid_touch_frame(const struct vt_frame *);
extern int sweet_hid_touch_ready(void);
extern void vermeer_debug_input_register(void (*)(void));
void vermeer_touch_start(void);
static void vt_status(void);

#define VT_SE 0x00a90000u
#define VT_WRAPPER 0x00ac0000u   /* geniqup@ac0000, spi4's wrapper */
#define VT_GCC 0x00100000u
#define VT_GPIO 0x0f100000u /* TLMM base f000000 + Kalama REG_BASE100000 */
#define VT_SMMU 0x15000000u
#define VT_SMMU_SIZE 0xd0000u
#define VT_QUP_SID 0xa3u         /* sm8550.dtsi geniqup@ac0000 iommus = <&apps_smmu 0xa3 0> */
#define VT_SPI_HZ 4000000u       /* conservative; GT9916 parts run at 10+ MHz */
#define VT_VOTE 0x52008u
#define VT_CLOCKS VTC_VOTES
#define VT_RCG 0x184f0u          /* gcc_qupv3_wrap1_s4_clk_src, DFS-managed */
/* Wrapper1 core2x/core/master/slave AHB, SE4. Never SE3. */
static const unsigned vt_halt[5]={0x23294,0x23284,0x2327c,0x23280,0x184e4};
/* The engine's DMA buffers: one 4 KiB page per direction, each translated on its own (kernel image pages are
 * not physically contiguous across pages here). */
static struct {uint8_t tx[4096],rx[4096];} __attribute__((aligned(16384))) vt_dma;
static struct {
    vm_offset_t se,gcc,gpio,wrapper,smmu;
    uint32_t vote_added,pins[4];
    bool pins_saved,root_forced,engine_touched;
    struct geni_spi spi;
    int last_err,smmu_slot;
    unsigned stage_detail,confirmed,nonzero,status_or,resets,cmd_ack,polls,frame_ready,frame_flag;
    uint32_t fw_state,raw[2];
    uint32_t _Atomic event_header;
    unsigned level,div;
    uint64_t sclk;
    struct vt_info info;
    struct vt_frame contacts;
    unsigned _Atomic stage;
    uint64_t _Atomic reports,bad,transactions,emitted,hid_retries;
    uint32_t _Atomic last_header;
    uint32_t _Atomic clock_vote,clock_master,clock_slave,clock_se,clock_root,clock_cfg,clock_dfs;
    uint32_t _Atomic clock_core2x,clock_core;
    uint32_t _Atomic engine_proto,reset_level,interface,irq_level;
    char _Atomic reason[48];
    uint8_t tx[4096],rx[4096];
} touch;
enum { VT_WAIT=1,VT_CLOCK,VT_FIRMWARE,VT_IDENTITY,VT_READY,VT_STOPPED };
static void vt_reason(const char *text)
{
    unsigned i;
    for(i=0;i+1<sizeof(touch.reason) && text[i];i++)touch.reason[i]=text[i];
    touch.reason[i]=0;
    printf("VERMEER TOUCH: %s\n",text);
}
static uint32_t vt_rd(vm_offset_t base,unsigned reg)
{
    uint32_t v=*(volatile uint32_t *)(base+reg);
    __asm__ volatile("dmb oshld" ::: "memory");return v;
}
static void vt_wr(vm_offset_t base,unsigned reg,uint32_t v)
{
    *(volatile uint32_t *)(base+reg)=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t vt_io_rd(void *ctx,uint32_t reg){return vt_rd((vm_offset_t)ctx,reg);}
static void vt_io_wr(void *ctx,uint32_t reg,uint32_t v){vt_wr((vm_offset_t)ctx,reg,v);}
static void vt_io_delay(void *ctx,unsigned us){(void)ctx;delay(us);}
static uint32_t vt_smmu_rd(void *ctx,unsigned reg){return vt_rd((vm_offset_t)ctx,reg);}
static void vt_smmu_wr(void *ctx,unsigned reg,uint32_t v){vt_wr((vm_offset_t)ctx,reg,v);}
static void vt_sleep(unsigned ms)
{
    uint64_t deadline;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
    assert_wait_deadline((event_t)&touch,THREAD_UNINT,deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
static void vt_cache(void *p,size_t n,int clean_only)
{
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    size_t line=4u<<((ctr>>16)&15u);
    for(uintptr_t a=(uintptr_t)p&~(line-1),end=(uintptr_t)p+n;a<end;a+=line) {
        if(clean_only)__asm__ volatile("dc cvac, %0"::"r"(a):"memory");
        else __asm__ volatile("dc civac, %0"::"r"(a):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static void vt_dma_clean(void *ctx,void *p,size_t n){(void)ctx;vt_cache(p,n,1);}
static void vt_dma_flush(void *ctx,void *p,size_t n){(void)ctx;vt_cache(p,n,0);}
/* The MMU's own answer (AT S1E1R, as mmu_kvtop); ml_vtophys if the probe faults. */
static uint64_t vt_vtop(const void *p)
{
    vm_offset_t va=(vm_offset_t)p;
    boolean_t enabled=ml_set_interrupts_enabled(FALSE);
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(par):"r"(va):"memory");
    ml_set_interrupts_enabled(enabled);
    if(!(par&1u))return (par&0x0000fffffffff000ull)|(va&0xfffu);
    return ml_vtophys(va);
}

static int vt_emit_frame(const struct vt_frame *frame)
{
    /* Once the controller report is acknowledged, retain it here until the
     * HID collection is built completely; never lose a release on ENOMEM. */
    for(unsigned i=0;i<125;i++) {
        int result=sweet_hid_touch_frame(frame);
        if(result) {
            touch.contacts=*frame;
            if(result==2)os_atomic_inc(&touch.emitted,relaxed);
            return 1;
        }
        os_atomic_inc(&touch.hid_retries,relaxed);vt_sleep(8);
    }
    printf("VERMEER TOUCH: complete HID frame unavailable after bounded retry\n");return 0;
}
static int vt_xfer(unsigned n)
{
    touch.last_err=geni_spi_transfer(&touch.spi,touch.tx,touch.rx,n,0);
    return touch.last_err==0;
}
static int vt_spi_read(uint32_t addr,uint8_t *out,unsigned n)
{
    if(!out || !n || n>sizeof(touch.tx)-8 || addr<0x10000 || addr>0x200000u-n)return 0;   /* one DMA page per direction */
    bzero(touch.tx,n+8);touch.tx[0]=0xf1;
    for(unsigned i=0;i<4;i++)touch.tx[1+i]=(uint8_t)(addr>>(24-i*8));
    touch.tx[5]=touch.tx[6]=touch.tx[7]=0xff;
    if(!vt_xfer(n+8))return 0;
    bcopy(touch.rx+8,out,n);os_atomic_inc(&touch.transactions,relaxed);return 1;
}
/* Berlin SPI write: 0xf0, 32-bit address, data (goodix_berlin_spi.c). */
static int vt_spi_write(uint32_t addr,const uint8_t *data,unsigned n)
{
    if(!data || !n || n>sizeof(touch.tx)-5 || addr<0x10000 || addr>0x200000u-n)return 0;
    touch.tx[0]=0xf0;
    for(unsigned i=0;i<4;i++)touch.tx[i+1]=(uint8_t)(addr>>(24-i*8));
    bcopy(data,touch.tx+5,n);
    if(!vt_xfer(n+5))return 0;
    os_atomic_inc(&touch.transactions,relaxed);return 1;
}
/* brl_send_cmd: struct goodix_ts_cmd {state, ack, len, cmd, data[10]} (16 bytes) with a u8-sum LE16 checksum over
 * len..data appended, written to misc.cmd_addr; the firmware sets ack = 0x80 when it took the command. */
static int vt_send_cmd(uint8_t cmd,const uint8_t *data,unsigned n)
{
    uint8_t buf[16]={0},back[16];
    if(!touch.info.cmd || n>8)return 0;
    buf[2]=(uint8_t)(n+4);buf[3]=cmd;
    for(unsigned i=0;i<n;i++)buf[4+i]=data[i];
    unsigned sum=0;for(unsigned i=2;i<2+n+2;i++)sum+=buf[i];
    buf[4+n]=(uint8_t)sum;buf[5+n]=(uint8_t)(sum>>8);
    for(unsigned tries=0;tries<6;tries++) {
        if(!vt_spi_write(touch.info.cmd,buf,sizeof(buf)))return 0;
        for(unsigned i=0;i<6;i++) {
            if(!vt_spi_read(touch.info.cmd,back,sizeof(back)))return 0;
            touch.cmd_ack=back[1];
            if(back[1]==0x80){vt_sleep(2);return 1;}
            if(back[1]==0x02 || back[1]==0x00){vt_sleep(1);continue;}
            vt_sleep(back[1]==0x03?10:1);break;
        }
    }
    return 0;
}
static int vt_clock_enable(void)
{
    touch.gcc=ml_io_map(VT_GCC,0x54000);
    if(!touch.gcc){vt_reason("GCC map failed");return 0;}
    uint32_t before=vt_rd(touch.gcc,VT_VOTE);
    touch.vote_added=VT_CLOCKS&~before;
    vt_wr(touch.gcc,VT_VOTE,before|VT_CLOCKS);
    for(unsigned i=0;i<1000;i++) {
        uint32_t votes=vt_rd(touch.gcc,VT_VOTE),core2x=vt_rd(touch.gcc,vt_halt[0]),core=vt_rd(touch.gcc,vt_halt[1]);
        uint32_t master=vt_rd(touch.gcc,vt_halt[2]),slave=vt_rd(touch.gcc,vt_halt[3]),se=vt_rd(touch.gcc,vt_halt[4]);
        uint32_t cmd=vt_rd(touch.gcc,VT_RCG);
        os_atomic_store(&touch.clock_vote,votes,relaxed);os_atomic_store(&touch.clock_master,master,relaxed);
        os_atomic_store(&touch.clock_core2x,core2x,relaxed);os_atomic_store(&touch.clock_core,core,relaxed);
        os_atomic_store(&touch.clock_slave,slave,relaxed);os_atomic_store(&touch.clock_se,se,relaxed);
        os_atomic_store(&touch.clock_root,cmd,relaxed);os_atomic_store(&touch.clock_cfg,vt_rd(touch.gcc,VT_RCG+4),relaxed);
        os_atomic_store(&touch.clock_dfs,vt_rd(touch.gcc,VT_RCG+0x14),relaxed);
        if(vtc_ready(votes,core2x,core,master,slave,se))return 1;
        /* Match clk_rcg2_set_force_enable when SE4's root is parked. This
         * requests the existing root only: no rate, parent, DFS or PLL edit.
         */
        if(i==100 && (votes&VT_CLOCKS)==VT_CLOCKS && (se&0x80000000u) &&
           (cmd&0x80000000u) && !(cmd&3u)) {
            touch.root_forced=true;vt_wr(touch.gcc,VT_RCG,cmd|2u);
        }
        delay(10);
    }
    vt_reason("SE4/AHB clock vote not ready");return 0;
}
/* clk-rcg2.c: source rate by select (gcc_parent_map_0: 0 TCXO, 1 GPLL0 600 MHz, 6 GPLL0 even 300 MHz), half-integer
 * divider, then M/N (N stored inverted). Same arithmetic as sheng's TouchBoard.h. */
static uint64_t vt_rcg_hz(uint32_t cfg,uint32_t mreg,uint32_t nreg)
{
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint64_t f=src==0?19200000ull:src==1?600000000ull:src==6?300000000ull:0;
    if(hid){f*=2;f/=hid+1;}
    if(mode){uint32_t m=mreg&0xffff,n=((~nreg)&0xffff)+m;if(n){f*=m;f/=n;}}
    return f;
}
/* Highest SPI rate not above target over the DFS levels (level selected through GENI_CLK_SEL). */
static int vt_choose_clock(unsigned target)
{
    int found=0;uint64_t best=0;
    unsigned os=touch.spi.oversampling?touch.spi.oversampling:1;
    int dfs=vt_rd(touch.gcc,VT_RCG+0x14)&1u;
    for(unsigned l=0;l<(dfs?8u:1u);l++) {
        uint64_t f=dfs?vt_rcg_hz(vt_rd(touch.gcc,VT_RCG+0x1c+4*l),vt_rd(touch.gcc,VT_RCG+0x5c+4*l),vt_rd(touch.gcc,VT_RCG+0x9c+4*l))
                      :vt_rcg_hz(vt_rd(touch.gcc,VT_RCG+4),vt_rd(touch.gcc,VT_RCG+8),vt_rd(touch.gcc,VT_RCG+0xc));
        if(!f)continue;
        uint64_t d=(f+(uint64_t)os*target-1)/((uint64_t)os*target);if(d<1)d=1;if(d>0xfff)continue;
        uint64_t actual=f/(os*d);if(actual>target)continue;
        if(!found || actual>best){found=1;best=actual;touch.level=l;touch.div=(unsigned)d;touch.sclk=f;}
    }
    return found;
}
/* Engine DMA buffers and the QUP stream's SMMU pass-through (sheng TouchProbe prepareDma). */
static int vt_dma_prepare(void)
{
    uint64_t tx=vt_vtop(vt_dma.tx),rx=vt_vtop(vt_dma.rx);
    if(!tx || !rx || (tx&0xfffu) || (rx&0xfffu) ||
       vt_vtop(vt_dma.tx+4095)!=tx+4095 || vt_vtop(vt_dma.rx+4095)!=rx+4095){vt_reason("DMA page translation failed");return 0;}
    bzero(&vt_dma,sizeof(vt_dma));vt_cache(&vt_dma,sizeof(vt_dma),0);
    touch.smmu=ml_io_map(VT_SMMU,VT_SMMU_SIZE);
    if(!touch.smmu){vt_reason("SMMU map failed");return 0;}
    uint64_t lo=tx<rx?tx:rx,hi=(tx<rx?rx:tx)+4096u;
    struct smmu_io io={(void *)touch.smmu,vt_smmu_rd,vt_smmu_wr};
    struct smmu_plan plan=smmu_plan_bypass(&io,VT_QUP_SID,lo,hi-lo);
    touch.smmu_slot=(int)plan.slot;
    const char *e=smmu_configure_bypass(&io,&plan,VT_QUP_SID);
    if(e){vt_reason(e);return 0;}
    touch.spi.use_dma=1;
    touch.spi.dma=(struct geni_dma){.ctx=NULL,.clean=vt_dma_clean,.clean_inval=vt_dma_flush,.inval=vt_dma_flush,
        .tx_virt=vt_dma.tx,.rx_virt=vt_dma.rx,.tx_phys=tx,.rx_phys=rx,.size=sizeof(vt_dma.tx)};
    return 1;
}
static int vt_engine_setup(void)
{
    touch.se=ml_io_map(VT_SE,0x4000);touch.gpio=ml_io_map(VT_GPIO,0x30000);touch.wrapper=ml_io_map(VT_WRAPPER,0x2000);
    if(!touch.se || !touch.gpio || !touch.wrapper){vt_reason("SE4/TLMM/QUP map failed");return 0;}
    touch.spi.se=(struct geni_io){(void *)touch.se,vt_io_rd,vt_io_wr,vt_io_delay};
    touch.spi.wrapper=(struct geni_io){(void *)touch.wrapper,vt_io_rd,vt_io_wr,vt_io_delay};
    if(geni_spi_read_info(&touch.spi)){vt_reason("SE4 info read failed");return 0;}
    os_atomic_store(&touch.engine_proto,touch.spi.info.protocol,relaxed);
    os_atomic_store(&touch.interface,touch.spi.info.if_disable,relaxed);
    if(touch.spi.info.protocol!=GENI_PROTO_SPI){vt_reason("SE4 firmware is not SPI");return 0;}
    /* Power-on, goodix_berlin_power_on order: reset (GPIO24, active low) held low as a GPIO output, iovdd
     * (L12B 1.8 V) through RPMh, 3 ms, avdd (L2B 3.2 V), 15 ms, reset high; the device confirm and the 100 ms
     * firmware boot follow once the bus is up. ABL leaves both rails off (r28s read all zeros, IRQ low).
     */
    uint32_t reset=vt_rd(touch.gpio,24*0x1000);
    vt_wr(touch.gpio,24*0x1000+4,0);
    vt_wr(touch.gpio,24*0x1000,(reset&~0x3fcu)|0x200u|(1u<<6));   /* GPIO function, output, 4 mA */
    touch.stage_detail=1;   /* the RSC is the next new MMIO: a freeze here names it */
    if(!kr_init() || !kr_ldo_on(1,"ldob12",1800)){vt_reason(kr.error?kr.error:"RPMh iovdd vote failed");return 0;}
    vt_sleep(3);
    if(!kr_ldo_on(0,"ldob2",3200)){vt_reason(kr.error?kr.error:"RPMh avdd vote failed");return 0;}
    touch.stage_detail=2;
    vt_sleep(15);   /* reset stays low until the SPI engine is ready, so the device confirm lands right after it */
    for(unsigned i=0;i<4;i++) {
        unsigned reg=(44+i)*0x1000;touch.pins[i]=vt_rd(touch.gpio,reg);
        /* Exact active DT: ALT1, 6mA; pulldown MISO/MOSI/SCLK, pullup CS. */
        uint32_t value=(touch.pins[i]&~0x3ffu)|4u|(2u<<6)|(i==3?3u:1u);
        vt_wr(touch.gpio,reg,value);
    }
    touch.pins_saved=true;
    if(touch.spi.info.fifo_disabled && !vt_dma_prepare())return 0;
    touch.engine_touched=true;
    touch.spi.poll_limit=200000;
    touch.spi.poll_only=2;   /* sheng: keep the top-level IRQ enable so the status registers stay visible to the poll */
    int r=geni_spi_init(&touch.spi);
    if(r){touch.last_err=r;vt_reason("GENI SPI init refused");return 0;}
    if(!vt_choose_clock(VT_SPI_HZ)){vt_reason("no SE4 clock level fits");return 0;}
    r=geni_spi_set_clock(&touch.spi,VT_SPI_HZ,(unsigned)touch.sclk,touch.level);
    if(!r)r=geni_spi_setup(&touch.spi,0,0);
    if(r){touch.last_err=r;vt_reason("GENI SPI clock/mode refused");return 0;}
    /* goodix_berlin_power_on, after reset release: 4 ms, then confirm the device (write 8 x 0xAA to the boot
     * option, read it back; up to 3 tries 5 ms apart), then 100 ms for the firmware to boot. Without the
     * confirm the controller can stay in its boot ROM: it answers the version read but never scans. */
    vt_wr(touch.gpio,24*0x1000+4,2);
    os_atomic_store(&touch.reset_level,vt_rd(touch.gpio,24*0x1000+4),relaxed);
    vt_sleep(4);
    touch.confirmed=0;
    for(unsigned tries=0;tries<3 && !touch.confirmed;tries++) {
        uint8_t back[8];
        static const uint8_t aa[8]={0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa};
        if(vt_spi_write(0x10000,aa,8) && vt_spi_read(0x10000,back,8) && !memcmp(aa,back,8))touch.confirmed=1;
        else vt_sleep(5);
    }
    if(!touch.confirmed){vt_reason("device boot confirmation failed");return 0;}
    vt_sleep(100);
    os_atomic_store(&touch.irq_level,vt_rd(touch.gpio,25*0x1000+4),relaxed);
    return 1;
}
static void vt_restore(void)
{
    /* Once the engine has been programmed (and maybe left mid-transfer) everything stays as it is until
     * reboot: disabling an active engine or its clocks could turn a recoverable error into a fault. */
    if(touch.engine_touched)return;
    if(touch.pins_saved)for(unsigned i=0;i<4;i++)vt_wr(touch.gpio,(44+i)*0x1000,touch.pins[i]);
    if(touch.gcc && touch.root_forced)vt_wr(touch.gcc,VT_RCG,vt_rd(touch.gcc,VT_RCG)&~2u);
    if(touch.gcc && touch.vote_added)
        vt_wr(touch.gcc,VT_VOTE,vt_rd(touch.gcc,VT_VOTE)&~touch.vote_added);
}
static int vt_identify(void)
{
    uint8_t version[28],length[2],data[1024];
    if(!vt_spi_read(0x10014,version,sizeof(version))){vt_reason("SPI version read failed");return 0;}
    /* What came back: all 00 or all ff means nobody answered (no power or still in reset). */
    os_atomic_store(&touch.last_header,vt_le32(version),relaxed);
    if(!vt_version_9916(version,sizeof(version))){vt_reason("not a GT9916 (version check)");return 0;}
    if(!vt_spi_read(0x10070,length,2)){vt_reason("SPI IC-info length read failed");return 0;}
    unsigned n=vt_le16(length);
    if(n<181 || n>=sizeof(data) || !vt_spi_read(0x10070,data,n) || !vt_info_decode(data,n,&touch.info)) {
        vt_reason("IC-info guard refused");return 0;
    }
    printf("VERMEER TOUCH: GT9916 verified, event=%08x axes=%ux%u\n",touch.info.event,touch.info.max_x,touch.info.max_y);
    return 1;
}
/* Host touch processing. Vermeer's GT9916 firmware leaves coordinates to Android's THP stack (Xiaomi's driver reads
 * thp_frame from misc.frame_data_addr when enable_touch_raw is set; in coordinate mode not one event ever arrived,
 * r30s-r32s), so, as sheng does for its Novatek panel, contacts are computed here from capacitance frames.
 * 0x90/0x82 selects firmware-baselined difference frames (brld_get_cap_data); a frame is ready when bit 7 of the
 * byte at frame_data_addr is set, the matrix (tx-major: [drv][sen], int16 LE) follows the frame head, attribute
 * and log blocks and 8 more bytes, and the flag is cleared by writing 0 (brl_after_event_handler).
 * Boot args: k70-touch-thr (signal threshold, default 200), k70-touch-map (bit0 swap which matrix axis is the
 * screen's long axis, bit1 mirror x, bit2 mirror y). */
static struct vtt_work vt_thp;
static uint8_t vt_fbuf[4096];
static struct { unsigned thr,map,frames,peaks,mode_ok; int max,max_t,max_r; } thp;
static int vt_thp_start(void)
{
    unsigned t=touch.info.tx,r=touch.info.rx;
    if(!touch.info.frame || !vtt_shape(t,r,touch.info.frame_skip))return 0;
    thp.thr=200;thp.map=0;
    PE_parse_boot_argn("k70-touch-thr",&thp.thr,sizeof(thp.thr));
    PE_parse_boot_argn("k70-touch-map",&thp.map,sizeof(thp.map));
    static const uint8_t diff=0x82,zero=0;
    thp.mode_ok=vt_send_cmd(0x90,&diff,1);
    if(thp.thr<1 || thp.thr>32768 || thp.map>7 || !thp.mode_ok ||
       !vt_spi_write(touch.info.frame,&zero,1))return 0;
    return 1;
}
/* 1: a new frame is in vt_m, 0: none ready yet, -1: bus failure */
static int vt_thp_frame(void)
{
    uint8_t flag=0;
    static const uint8_t zero=0;
    if(!vt_spi_read(touch.info.frame,&flag,1))return -1;
    if(!(flag&0x80u))return 0;
    unsigned nodes=touch.info.tx*touch.info.rx,off=touch.info.frame_skip+8;
    if(!vt_spi_read(touch.info.frame,vt_fbuf,off+nodes*2))return -1;
    if(!vt_spi_write(touch.info.frame,&zero,1))return -1;
    if(!vtt_decode(&vt_thp,vt_fbuf,off+nodes*2,touch.info.tx,touch.info.rx,touch.info.frame_skip))return -1;
    thp.frames++;
    return 1;
}
static int vt_thp_process(void)
{
    struct vt_frame frame={0};
    if(!vtt_process(&vt_thp,touch.info.tx,touch.info.rx,thp.thr,thp.map,&touch.contacts,&frame))return 0;
    if(!vt_emit_frame(&frame))return 0;
    os_atomic_inc(&touch.reports,relaxed);return 1;
}
static void vt_thread(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    /* NativeHidInput registers its sink once IOHIDFamily has matched it. */
    for(unsigned i=0;i<1800 && !sweet_hid_touch_ready();i++)vt_sleep(100);
    if(!sweet_hid_touch_ready()){vt_reason("HID touch sink never registered");goto stopped;}
    os_atomic_store(&touch.stage,VT_CLOCK,relaxed);
    if(!vt_clock_enable())goto stopped;
    os_atomic_store(&touch.stage,VT_FIRMWARE,relaxed);
    if(!vt_engine_setup())goto stopped;
    os_atomic_store(&touch.stage,VT_IDENTITY,relaxed);
    if(!vt_identify())goto stopped;
    os_atomic_store(&touch.stage,VT_READY,relaxed);
    vt_reason("GT9916 ready");
    if(!vt_thp_start()){vt_reason("THP mode/geometry refused");goto stopped;}
    printf("VERMEER TOUCH: physical GT9916 input ready (SE DMA, RPMh, THP, atomic HID)\n");
    for(;;) {
        int r=vt_thp_frame();
        if(r<0){vt_reason("SPI frame read failed");goto stopped;}
        if(!r){vt_sleep(4);continue;}
        if(!vt_thp_process()){vt_reason("HID frame dispatch failed");goto stopped;}
        vt_sleep(4);
    }
stopped:
    { struct vt_frame empty={0};
      if(!vt_emit_frame(&empty))vt_reason("HID release pending at stop"); }
    vt_restore();os_atomic_store(&touch.stage,VT_STOPPED,relaxed);
    thread_terminate(current_thread());
}
void vermeer_touch_start(void)
{
    int enabled=0;DTEntry entry;const void *value=NULL;unsigned size=0;
    if(!PE_parse_boot_argn("vermeer_touch",&enabled,sizeof(enabled)) || enabled!=1)return;
    if(SecureDTLookupEntry(NULL,"/",&entry)!=kSuccess ||
       SecureDTGetProperty(entry,"physical-model",&value,&size)!=kSuccess ||
       size!=sizeof("Redmi K70") || memcmp(value,"Redmi K70",size)) {
        printf("VERMEER TOUCH: physical-model guard refused\n");return;
    }
    os_atomic_store(&touch.stage,VT_WAIT,relaxed);vermeer_debug_input_register(vt_status);
    thread_t thread=THREAD_NULL;
    kern_return_t result=kernel_thread_start(vt_thread,NULL,&thread);
    if(result==KERN_SUCCESS)thread_deallocate(thread);
    else os_atomic_store(&touch.stage,VT_STOPPED,relaxed);
}
static void vt_status(void)
{
    printf("VERMEER TOUCH: stage=%u reports=%llu bad=%llu spi=%llu emitted=%llu retries=%llu\n",
        os_atomic_load(&touch.stage,relaxed),os_atomic_load(&touch.reports,relaxed),
        os_atomic_load(&touch.bad,relaxed),os_atomic_load(&touch.transactions,relaxed),
        os_atomic_load(&touch.emitted,relaxed),os_atomic_load(&touch.hid_retries,relaxed));
    printf("VERMEER TOUCH: clock-snapshot vote=%08x core=%08x se4=%08x\n",
        os_atomic_load(&touch.clock_vote,relaxed),os_atomic_load(&touch.clock_core,relaxed),
        os_atomic_load(&touch.clock_se,relaxed));
    printf("VERMEER TOUCH: spi-snapshot protocol=%u interface=%08x reset=%u irq=%u\n",
        os_atomic_load(&touch.engine_proto,relaxed),os_atomic_load(&touch.interface,relaxed),
        os_atomic_load(&touch.reset_level,relaxed),os_atomic_load(&touch.irq_level,relaxed));
}
#endif
