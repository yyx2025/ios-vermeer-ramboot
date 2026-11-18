/* Pre-mount, checked USB receive into guarded external RAM, then compressed md0.
 * This code has no UFS, GPT, persistent console or physical-storage operations.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <libkern/crypto/sha2.h>
#include "vermeer_packed_store.h"
#include "vermeer_ram_receive.h"
#include "vermeer_ram_pump.h"
#include "lzfse/lzfse_internal.h"

#define STREAM_BASE UINT64_C(0x940000000)
#define STREAM_BYTES UINT64_C(0x180000000)
/* Larger COW arenas still must fit after the packed root in the same 6 GiB
 * external aperture; the checks in start_receiver reject every overlap. */
#define STREAM_OVERLAY_MAX UINT64_C(0x80000000)
#define STREAM_OVERLAY_MIN UINT64_C(0x10000000)
struct stream_config {
    char magic[8];
    uint64_t packed_bytes,base,bytes,overlay_bytes;
    uint8_t sha256[32];
};
_Static_assert(sizeof(struct stream_config)==72,"RAM stream ADT layout");
static struct stream_config config;
static struct vrs_store store;
static struct vrx_state receive;
static lzfse_decoder_state decoder;
static SHA256_CTX transfer_hash;
static vm_offset_t area,cache,page_index;
static _Atomic int ready,receive_failed;
static _Atomic uint64_t receive_progress;
static int sleep_channel;
/* Called only by the BSD bootstrap thread. Never restart an owned DMA path. */
static int start_attempted,start_error;
static int receive_prepared;
static const char *root_failure_stage="config-or-aperture";
static int decode_status,decode_complete,decode_failed;
static size_t decode_written,decode_consumed;
const char *vermeer_ram_root_failure_stage(void) { return root_failure_stage; }
LCK_GRP_DECLARE(stream_group,"vermeer-ram-root");
LCK_MTX_DECLARE(stream_lock,&stream_group);
extern dev_t mdevadd(int,uint64_t,unsigned int,int);
/* BSD's exported pmap header omits this Mach-private declaration.
 * Match XNU-8796; NOWAIT returns table resource shortages to this caller. */
extern kern_return_t pmap_enter_options(pmap_t,vm_map_offset_t,ppnum_t,
    vm_prot_t,vm_prot_t,unsigned int,boolean_t,unsigned int,void *);
extern ppnum_t pmap_find_phys(pmap_t,vm_map_offset_t);
extern void vermeer_usb_debug_start(void);
extern int vermeer_usb_running(void);
extern int vermeer_usb_ram_step(void);
extern int vermeer_usb_connected(void);
extern int vermeer_usb_ram_handoff(void);
extern int vermeer_usb_logs_drained(void);
extern void vermeer_usb_busy_wait(unsigned);
extern void vermeer_usb_set_ram_receiver(int (*)(const void *,unsigned));
int vermeer_ram_stream_boot(void);
int vermeer_ram_stream_start(void);
int vermeer_packed_active(int);
int vermeer_packed_buf(struct buf *,uint64_t,void *);
int vermeer_packed_uio(struct uio *);
int vermeer_packed_synchronize(int);

static void hash_hex(const uint8_t *hash,char *text)
{
    static const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<32;i++) {
        text[2*i]=digits[hash[i]>>4];text[2*i+1]=digits[hash[i]&15];
    }
    text[64]=0;
}
static void hash_mismatch(const char *stage,const uint8_t *expected,const uint8_t *actual)
{
    char want[65],got[65];hash_hex(expected,want);hash_hex(actual,got);
    printf("VERMEER ROOT: %s expected=%s\n",stage,want);
    printf("VERMEER ROOT: %s actual=%s\n",stage,got);
}
static int index_sha(void *ctx,const void *data,size_t n,const uint8_t *expected)
{
    (void)ctx;SHA256_CTX hash;uint8_t value[32];
    SHA256_Init(&hash);SHA256_Update(&hash,data,n);SHA256_Final(value,&hash);
    if (memcmp(value,expected,sizeof(value))) {
        root_failure_stage="index-sha256-mismatch";
        hash_mismatch(root_failure_stage,expected,value);return 0;
    }
    printf("VERMEER ROOT: index SHA256 matched; %lu bytes\n",(unsigned long)n);
    return 1;
}
static int decode_block(void *ctx,void *dst,size_t n,const void *src,size_t stored)
{
    (void)ctx;memset(&decoder,0,sizeof(decoder));
    decoder.src=decoder.src_begin=src;decoder.src_end=decoder.src+stored;
    decoder.dst=decoder.dst_begin=dst;decoder.dst_end=decoder.dst+n;
    int result=lzfse_decode(&decoder);
    decode_status=result;decode_complete=decoder.end_of_stream;
    decode_written=(size_t)(decoder.dst-decoder.dst_begin);
    decode_consumed=(size_t)(decoder.src-decoder.src_begin);
    int ok=result==LZFSE_STATUS_OK && decoder.end_of_stream &&
        decode_written==n && decoder.src==decoder.src_end;
    if (!ok) {
        decode_failed=1;
        root_failure_stage="block-decode";
        printf("VERMEER ROOT: decode failed status=%d eos=%d output=%lu/%lu input=%lu/%lu\n",
            result,decode_complete,(unsigned long)decode_written,(unsigned long)n,
            (unsigned long)decode_consumed,(unsigned long)stored);
    }
    return ok;
}
static int receive_begin(void *ctx,uint64_t bytes,const uint8_t *sha)
{
    (void)ctx;
    if (bytes!=config.packed_bytes || memcmp(sha,config.sha256,32)) return EINVAL;
    SHA256_Init(&transfer_hash);
    printf("VERMEER RAM: receiving complete packed root, %llu bytes; volatile only\n",bytes);
    return 0;
}
static int verify_sha_provider(void)
{
    static uint8_t data[4096];
    static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    static const uint8_t pattern[32]={0xd4,0x1d,0x43,0x8c,0x37,0x91,0x10,0xc7,0xf7,0xb2,0xc5,0x61,0xb1,0xf0,0x4f,0x26,0xc1,0xb4,0x54,0x91,0x10,0x79,0x1f,0x8e,0x02,0x2f,0x48,0x97,0x42,0x80,0xc1,0x3e};
    SHA256_CTX hash;uint8_t actual[32];root_failure_stage="sha256-selftest";
    SHA256_Init(&hash);SHA256_Update(&hash,"abc",3);SHA256_Final(actual,&hash);
    if(memcmp(actual,abc,32)) {hash_mismatch(root_failure_stage,abc,actual);return EIO;}
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*31+7);
    SHA256_Init(&hash);
    for(size_t off=0;off<sizeof(data);) {
        size_t n=sizeof(data)-off;if(n>17)n=17;
        SHA256_Update(&hash,data+off,n);off+=n;
    }
    SHA256_Final(actual,&hash);
    if(memcmp(actual,pattern,32)) {hash_mismatch(root_failure_stage,pattern,actual);return EIO;}
    printf("VERMEER ROOT: SHA256 selftest matched; abc and fragmented 4096-byte pattern\n");return 0;
}
struct root_probe_header {char magic[8];uint32_t count,entry_bytes;uint64_t packed_bytes;uint8_t sha256[32];};
struct root_probe_entry {uint64_t offset;uint32_t bytes,reserved;uint8_t sha256[32];};
_Static_assert(sizeof(struct root_probe_header)==56,"root probe header layout");
_Static_assert(sizeof(struct root_probe_entry)==48,"root probe entry layout");
static int verify_root_probes(DTEntry chosen)
{
    const void *blob=NULL;unsigned size=0;struct root_probe_header header;
    root_failure_stage="root-probes-config";
    if(SecureDTGetProperty(chosen,"vermeer-ram-probes",&blob,&size)!=kSuccess || size<sizeof(header))return EINVAL;
    memcpy(&header,blob,sizeof(header));
    if(memcmp(header.magic,"VMRP0001",8) || !header.count || header.count>128 ||
       header.entry_bytes!=sizeof(struct root_probe_entry) ||
       size!=sizeof(header)+header.count*sizeof(struct root_probe_entry) ||
       header.packed_bytes!=config.packed_bytes || memcmp(header.sha256,config.sha256,32))return EINVAL;
    printf("VERMEER ROOT: RAM witness v4; checking %u mapped 16 KiB samples\n",header.count);
    for(unsigned i=0;i<header.count;i++) {
        struct root_probe_entry entry;SHA256_CTX hash;uint8_t actual[32];
        memcpy(&entry,(const uint8_t *)blob+sizeof(header)+i*sizeof(entry),sizeof(entry));
        if(entry.reserved || entry.bytes!=16384 || (entry.offset&16383) ||
           entry.offset>config.packed_bytes || entry.bytes>config.packed_bytes-entry.offset)return EINVAL;
        root_failure_stage="root-map-address";
        uint64_t wanted=STREAM_BASE+entry.offset;
        uint64_t physical=((uint64_t)pmap_find_phys(kernel_pmap,area+entry.offset)<<PAGE_SHIFT);
        if(physical!=wanted) {
            printf("VERMEER ROOT: mapping mismatch offset=%llx va=%llx expected-pa=%llx actual-pa=%llx\n",
                entry.offset,(uint64_t)(area+entry.offset),wanted,physical);return EIO;
        }
        root_failure_stage="mapped-root-probe-mismatch";
        SHA256_Init(&hash);SHA256_Update(&hash,(const void *)(area+entry.offset),entry.bytes);SHA256_Final(actual,&hash);
        if(memcmp(actual,entry.sha256,32)) {
            printf("VERMEER ROOT: mapped sample mismatch probe=%u offset=%llx va=%llx pa=%llx\n",
                i,entry.offset,(uint64_t)(area+entry.offset),physical);
            const uint8_t *head=(const void *)(area+entry.offset);
            printf("VERMEER ROOT: sample first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                head[0],head[1],head[2],head[3],head[4],head[5],head[6],head[7]);
            hash_mismatch(root_failure_stage,entry.sha256,actual);return EIO;
        }
    }
    printf("VERMEER ROOT: mapped RAM probes and physical addresses matched; full SHA still required\n");return 0;
}
#ifndef VMRW_WINDOW_BYTES
#define VMRW_WINDOW_BYTES UINT64_C(0x4000000)
#endif
/* Full coverage is independent of the sparse handoff witnesses. Both hashes
 * consume the same bounded snapshot in managed RAM; the full-file gate stays
 * mandatory before any index/decode/HFS operation. */
static int verify_root_windows(DTEntry chosen) __attribute__((noinline));
static int verify_root_windows(DTEntry chosen)
{
    const void *blob=NULL;unsigned size=0;struct root_probe_header header;
    root_failure_stage="root-windows-config";
    if(SecureDTGetProperty(chosen,"vermeer-ram-windows",&blob,&size)!=kSuccess || size<sizeof(header))return EINVAL;
    memcpy(&header,blob,sizeof(header));
    uint64_t count=(config.packed_bytes+VMRW_WINDOW_BYTES-1)/VMRW_WINDOW_BYTES;
    if(memcmp(header.magic,"VMRW0001",8) || !count || count>128 || header.count!=count ||
       header.entry_bytes!=sizeof(struct root_probe_entry) ||
       size!=sizeof(header)+header.count*sizeof(struct root_probe_entry) ||
       header.packed_bytes!=config.packed_bytes || memcmp(header.sha256,config.sha256,32) || !cache)return EINVAL;
    printf("VERMEER ROOT: RAM consistency v5; verifying %u contiguous windows through managed snapshots\n",header.count);
    uint64_t covered=0;
    for(unsigned i=0;i<header.count;i++) {
        struct root_probe_entry entry;SHA256_CTX window_hash;uint8_t actual[32];
        memcpy(&entry,(const uint8_t *)blob+sizeof(header)+i*sizeof(entry),sizeof(entry));
        uint64_t remaining=config.packed_bytes-covered;
        uint64_t wanted=remaining>VMRW_WINDOW_BYTES?VMRW_WINDOW_BYTES:remaining;
        root_failure_stage="root-windows-config";
        if(entry.reserved || entry.offset!=covered || entry.bytes!=wanted || !entry.bytes)return EINVAL;
        root_failure_stage="root-window-read";SHA256_Init(&window_hash);
        for(uint64_t off=entry.offset;off<entry.offset+entry.bytes;) {
            uint64_t left=entry.offset+entry.bytes-off;
            size_t n=(size_t)(left>VRS_BLOCK?VRS_BLOCK:left);
            memcpy((void *)cache,(const void *)(area+off),n);
            SHA256_Update(&window_hash,(const void *)cache,n);
            SHA256_Update(&transfer_hash,(const void *)cache,n);off+=n;
            if(off==VRS_BLOCK || !(off&UINT64_C(0x3ffffff)) || off==config.packed_bytes)
                printf("VERMEER RAM: SHA256 verified %llu/%llu bytes\n",off,config.packed_bytes);
        }
        SHA256_Final(actual,&window_hash);
        if(memcmp(actual,entry.sha256,32)) {
            root_failure_stage="mapped-root-window-mismatch";
            printf("VERMEER ROOT: full window mismatch window=%u offset=%llx bytes=%u\n",i,entry.offset,entry.bytes);
            hash_mismatch(root_failure_stage,entry.sha256,actual);return EIO;
        }
        covered+=entry.bytes;
    }
    if(covered!=config.packed_bytes)return EINVAL;
    printf("VERMEER ROOT: all contiguous RAM windows matched; independent full SHA still required\n");return 0;
}
static int receive_data(void *ctx,uint64_t offset,const void *data,size_t n)
{
    (void)ctx;
    if (offset>config.packed_bytes || n>config.packed_bytes-offset) return EINVAL;
    memcpy((void *)(area+offset),data,n);SHA256_Update(&transfer_hash,data,n);
    receive_progress=offset+n;
    return 0;
}
static int receive_finish(void *ctx)
{
    (void)ctx;uint8_t hash[32],hfs[512];
    root_failure_stage="full-sha256-final";
    SHA256_Final(hash,&transfer_hash);
    if (memcmp(hash,config.sha256,32)) {
        root_failure_stage="full-sha256-mismatch";
        hash_mismatch(root_failure_stage,config.sha256,hash);return EIO;
    }
    printf("VERMEER ROOT: complete file SHA256 matched; root-check v3\n");
    uint64_t overlay_off=(config.packed_bytes+PAGE_MASK)&~(uint64_t)PAGE_MASK;
    root_failure_stage="packed-open";
    printf("VERMEER ROOT: opening packed index; packed=%llu overlay-offset=%llx\n",
        config.packed_bytes,overlay_off);
    int error=vrs_open(&store,(void *)area,(size_t)config.packed_bytes,(void *)cache,
        (uint32_t *)page_index,VRS_MAX_LOGICAL/VRS_PAGE,(void *)(area+overlay_off),
        (uint32_t)(config.overlay_bytes/VRS_PAGE),index_sha,decode_block,NULL);
    if (error) {
        printf("VERMEER ROOT: open failed vrs=%d; stage=%s\n",error,root_failure_stage);
        return EIO;
    }
    printf("VERMEER ROOT: packed index opened; logical=%llu blocks=%u\n",store.logical_bytes,store.blocks);
    root_failure_stage="hfs-block-read";decode_status=0;decode_complete=0;decode_failed=0;
    decode_written=decode_consumed=0;
    error=vrs_read(&store,1024,hfs,sizeof(hfs));
    if (error) {
        if (error==VRS_CORRUPT && !decode_failed) root_failure_stage="hfs-block-crc";
        printf("VERMEER ROOT: HFS read failed vrs=%d; stage=%s decode=%d eos=%d output=%lu input=%lu\n",
            error,root_failure_stage,decode_status,decode_complete,
            (unsigned long)decode_written,(unsigned long)decode_consumed);
        return EIO;
    }
    printf("VERMEER ROOT: HFS header read; signature=%02x%02x attributes=%02x%02x%02x%02x\n",
        hfs[0],hfs[1],hfs[4],hfs[5],hfs[6],hfs[7]);
    root_failure_stage="hfs-header-policy";
    if ((hfs[0]!='H' || (hfs[1]!='X' && hfs[1]!='+')) || !(hfs[6]&0x20) ||
        (store.logical_bytes&4095) || (store.logical_bytes>>12)>UINT32_MAX) return EINVAL;
    printf("VERMEER RAM: complete packed SHA256/index/HFS header verified; logical %llu; overlay %llu\n",
        store.logical_bytes,config.overlay_bytes);
    ready=1;wakeup(&sleep_channel);return 0;
}
static void receive_reply(void *ctx,uint32_t seq,uint64_t received,int error)
{
    (void)ctx;
    printf("VMRAM %s %u %llu\n",error?"ERROR":"ACK",seq,received);
    if (error) { receive_failed=1;wakeup(&sleep_channel); }
}
static int receiver_bytes(const void *data,unsigned n)
{
    if (!data) { receive_failed=1;wakeup(&sleep_channel);return -1; }
    /* CDC can enumerate while the aperture is being mapped. No bytes may
     * reach it until mapping completes and READY is printed. */
    if (!receive_prepared) { receive_failed=1;return -1; }
    int result=vrx_feed(&receive,data,n);
    return result<0?-1:(receive.complete?1:0);
}

static int pump_step(void *ctx) { (void)ctx;return vermeer_usb_ram_step(); }
static int pump_state(void *ctx)
{
    if (receive_failed || !vermeer_usb_running())return -1;
    return ctx?ready:vermeer_usb_connected();
}
static uint64_t pump_progress(void *ctx) { return ctx?receive_progress:0; }
static int pump_drain_state(void *ctx)
{
    (void)ctx;
    if (receive_failed || !vermeer_usb_running())return -1;
    return vermeer_usb_logs_drained();
}
static uint64_t pump_now(void *ctx) { (void)ctx;return mach_absolute_time(); }
static void pump_idle(void *ctx) { (void)ctx;vermeer_usb_busy_wait(1000); }
static int pump_wait(int receiving,unsigned seconds)
{
    uint64_t ticks;
    clock_interval_to_absolutetime_interval(seconds,NSEC_PER_SEC,&ticks);
    struct vrp_ops ops={receiving?(void *)&receive:NULL,pump_step,receiving==2?pump_drain_state:pump_state,
        pump_progress,pump_now,pump_idle};
    int result=vrp_wait(&ops,ticks);
    return result==VRP_COMPLETE?0:(result==VRP_TIMEOUT?ETIMEDOUT:EIO);
}

static int start_receiver(void)
{
    DTEntry chosen;const void *blob=NULL;unsigned size=0;
    if (SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
        SecureDTGetProperty(chosen,"vermeer-ram-config",&blob,&size)!=kSuccess || size!=sizeof(config))return EINVAL;
    memcpy(&config,blob,sizeof(config));
    if (memcmp(config.magic,"VMRC0001",8) || config.base!=STREAM_BASE || config.bytes!=STREAM_BYTES ||
        config.overlay_bytes<STREAM_OVERLAY_MIN || config.overlay_bytes>STREAM_OVERLAY_MAX ||
        (config.overlay_bytes&PAGE_MASK) || config.packed_bytes<VRS_ALIGN ||
        config.packed_bytes>config.bytes-config.overlay_bytes ||
        ((config.packed_bytes+PAGE_MASK)&~(uint64_t)PAGE_MASK)>config.bytes-config.overlay_bytes)return EINVAL;
    printf("VERMEER RAM: VMRC0001 config accepted; VMRAM001 complete root expected\n");
    int preloaded=0,error=0;
    PE_parse_boot_argn("vermeer_ram_preloaded",&preloaded,sizeof(preloaded));
    if (preloaded) {
        uint32_t value=0;
        if (preloaded!=1 || SecureDTGetProperty(chosen,"vermeer-ram-preloaded",&blob,&size)!=kSuccess || size!=sizeof(value))return EINVAL;
        memcpy(&value,blob,sizeof(value));if (value!=1)return EINVAL;
        printf("VERMEER RAM: loader supplied compressed root; no XNU USB receiver required\n");
        error=verify_sha_provider();if(error)return error;
    } else {
        struct vrx_ops ops={NULL,receive_begin,receive_data,receive_finish,receive_reply};
        vrx_init(&receive,&ops);
        vermeer_usb_set_ram_receiver(receiver_bytes);vermeer_usb_debug_start();
        if (!vermeer_usb_running())return ENXIO;
        printf("VERMEER RAM: polling CDC enumeration before root mapping/network; waiting for computer\n");
        error=pump_wait(0,240);
        if (error)return error;
    }
    /* This aperture is outside the retained 2GiB XNU physical allocator.
     * The shim supplies this ADT property only after the live 8GiB FDT guard.
     * Use normal coherent RAM attributes, never ml_io_map's device attributes
     * or the small early-I/O submap, for multi-GiB compressed bytes.
     */
    printf("VERMEER RAM: reserving external root aperture before networking\n");
    if (kmem_alloc(kernel_map,&area,STREAM_BYTES,KMA_DATA|KMA_PAGEABLE|KMA_PERMANENT,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS ||
        kmem_alloc(kernel_map,&cache,VRS_BLOCK,KMA_DATA|KMA_ZERO|KMA_PERMANENT,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS ||
        kmem_alloc(kernel_map,&page_index,(VRS_MAX_LOGICAL/VRS_PAGE)*sizeof(uint32_t),
        KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
    printf("VERMEER RAM: mapping external root aperture\n");
    /* Service control requests/logs between bounded mapping batches. Mapping
     * stays cacheable and outside the 2GiB managed allocator. */
    const uint64_t batch=UINT64_C(0x1000000);
    for (uint64_t off=0;off<STREAM_BYTES;off+=batch) {
        if (!preloaded && vermeer_usb_ram_step()<0)return EIO;
        /* pmap_map uses the same enter routine with options=0, which can
         * sleep indefinitely while growing tables. Preserve page geometry,
         * coherent RAM attributes and mapping ownership, but report a
         * resource shortage before touching any unmapped root bytes. */
        if (!(off&UINT64_C(0x3ffffff)))
            printf("VERMEER RAM: mapping %llu/6144 MiB; page=%lu\n",
                off>>20,(unsigned long)PAGE_SIZE);
        for (uint64_t page=off;page<off+batch;page+=PAGE_SIZE) {
            kern_return_t kr=pmap_enter_options(kernel_pmap,area+page,
                (ppnum_t)((STREAM_BASE+page)>>PAGE_SHIFT),
                VM_PROT_READ|VM_PROT_WRITE,VM_PROT_NONE,VM_WIMG_COPYBACK,
                FALSE,PMAP_OPTIONS_NOWAIT,NULL);
            if (kr!=KERN_SUCCESS) {
                printf("VERMEER RAM: mapping failed kr=%d offset=%llx va=%llx pa=%llx\n",
                    kr,page,(uint64_t)(area+page),STREAM_BASE+page);
                return kr==KERN_RESOURCE_SHORTAGE?ENOMEM:EIO;
            }
        }
        if (!((off+batch)&UINT64_C(0x3ffffff)))
            printf("VERMEER RAM: mapped %llu/6144 MiB\n",(off+batch)>>20);
        if (!((off+batch)&UINT64_C(0x3fffffff)))
            printf("VERMEER RAM: mapped %llu/6 GiB\n",(off+batch)>>30);
    }
    printf("VERMEER RAM: external RAM mapped, base=%llx bytes=%llx; XNU allocator remains 2GiB\n",
        config.base,config.bytes);
    if (preloaded) {
        printf("VERMEER RAM: verifying preloaded full-root SHA256 before use\n");
        error=verify_root_probes(chosen);if(error)return error;
        printf("VERMEER RAM: SHA256 init begin; scalar provider v2\n");
        SHA256_Init(&transfer_hash);
        printf("VERMEER RAM: SHA256 init complete; first read/hash next\n");
        error=verify_root_windows(chosen);if(error)return error;
        printf("VERMEER RAM: SHA256 final/index/HFS verification begin\n");
        error=receive_finish(NULL);
        if (error)return error;
        receive_prepared=1;
        printf("VERMEER RAM: preloaded root ready; block decompression stays in RAM\n");
        return 0;
    }
    receive_prepared=1;
    printf("VERMEER RAM: READY VMRX0001; waiting for checked complete root; no storage writes\n");
    error=pump_wait(1,600);
    if (error)return error;
    printf("VERMEER RAM: complete root received before networking; bootstrap poll v1\n");
    /* Deliver the COMMIT ACK through the same polled owner before relying on
     * worker scheduling for subsequent diagnostic output. */
    error=pump_wait(2,30);
    if (error)return error;
    return vermeer_usb_ram_handoff()?EIO:0;
}
int vermeer_ram_stream_start(void)
{
    int enabled=0;
    if (!PE_parse_boot_argn("vermeer_ram_stream",&enabled,sizeof(enabled)) || enabled!=1)return 0;
    if (start_attempted)return start_error;
    start_attempted=1;start_error=EIO;
    int preloaded=0;PE_parse_boot_argn("vermeer_ram_preloaded",&preloaded,sizeof(preloaded));
    if (preloaded)printf("VERMEER RAM: checking loader root before network initialization\n");
    else printf("VERMEER RAM: starting USB before network initialization\n");
    start_error=start_receiver();
    return start_error;
}
int vermeer_ram_stream_boot(void)
{
    int enabled=0;
    if (!PE_parse_boot_argn("vermeer_ram_stream",&enabled,sizeof(enabled)) || enabled!=1)return 0;
    int error=vermeer_ram_stream_start();
    if(error)return error;
    printf("VERMEER RAM: network initialization complete; finishing checked root receive\n");
    if (!ready || receive_failed)return EIO;
    if (mdevadd(0,(uint64_t)cache>>12,(unsigned)(store.logical_bytes>>12),0)==NODEV)return ENXIO;
    printf("VERMEER RAM: compressed md0 registered; RAM writes limited to %llu bytes\n",config.overlay_bytes);
    return 0;
}
int vermeer_packed_active(int devid) { return devid==0 && ready; }
static int io_error(int error)
{ return error==VRS_FULL?ENOSPC:(error==VRS_INVALID?EINVAL:EIO); }

/* These offsets come from the verified v37 root's Catalog overflow extent,
 * not from a guess about physical RAM. Runtime HFS relocation could change
 * them. Samples identify the block-device bytes, and never assert that an
 * uninitialized B-tree node is evidence of a lost write. */
#define VRS_DIAG_CATALOG_FIRST UINT64_C(0x2e76a0000)
#define VRS_DIAG_CATALOG_END UINT64_C(0x2e76d0000)
#define VRS_DIAG_ATTR_NODE21 UINT64_C(0xd94000)
#define VRS_DIAG_NODE_BYTES 8192u
#define VRS_DIAG_NODES 25u
#define VRS_DIAG_SAMPLES 4u
#define VRS_DIAG_CAPACITY_STEP (64u*1024u*1024u/VRS_PAGE)
struct vrs_node_witness {
    uint64_t write_sequence;
    uint32_t crc,slot,reads,writes,mismatches;
};
struct vrs_node_sample {
    uint64_t offset,write_sequence,physical,expected_physical;
    uint32_t node,fileid,slot,last_slot,crc,last_crc,records;
    int kind,zero,match;
};
struct vrs_io_report {
    uint64_t sequence,offset;
    size_t bytes;
    uint32_t used,capacity,errors,sample_count;
    int read,error,emit_capacity,emit_error;
    struct vrs_node_sample samples[VRS_DIAG_SAMPLES];
};
static struct vrs_node_witness vrs_node_witnesses[VRS_DIAG_NODES];
static uint64_t vrs_diag_sequence;
static uint32_t vrs_diag_bucket,vrs_diag_errors;
static int vrs_sync_reported;

static uint32_t vrs_diag_crc(const uint8_t *p,size_t bytes)
{
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<bytes;i++) {
        crc^=p[i];
        for (unsigned j=0;j<8;j++) crc=(crc>>1)^(UINT32_C(0xedb88320)&(0u-(crc&1u)));
    }
    return ~crc;
}
static int vrs_diag_zero(const uint8_t *p,size_t bytes)
{
    for (size_t i=0;i<bytes;i++)if(p[i])return 0;
    return 1;
}
/* Called under stream_lock after the actual transfer. A witness is updated
 * only after a successful write and observes the full existing overlay node.
 * Full-node reads can then distinguish untouched base bytes, changed slots,
 * a failed copy, and a change since the last successful write. */
static void vrs_diag_locked(struct vrs_io_report *r,uint64_t off,const void *data,
                           size_t bytes,int read,int error)
{
    memset(r,0,sizeof(*r));r->sequence=++vrs_diag_sequence;r->offset=off;
    r->bytes=bytes;r->read=read;r->error=error;r->used=store.overlay_used;
    r->capacity=store.overlay_capacity;
    uint32_t bucket=store.overlay_used/VRS_DIAG_CAPACITY_STEP;
    if(bucket!=vrs_diag_bucket){vrs_diag_bucket=bucket;r->emit_capacity=1;}
    if(error) {
        r->errors=++vrs_diag_errors;
        r->emit_error=r->errors<=8 || (r->errors<=65536 && !(r->errors&(r->errors-1)));
        return;
    }
    if(!bytes || off>store.logical_bytes || bytes>store.logical_bytes-off)return;
    uint64_t end=off+bytes;
    if ((end<=VRS_DIAG_CATALOG_FIRST || off>=VRS_DIAG_CATALOG_END) &&
        (end<=VRS_DIAG_ATTR_NODE21 || off>=VRS_DIAG_ATTR_NODE21+VRS_DIAG_NODE_BYTES))return;
    for(unsigned i=0;i<VRS_DIAG_NODES;i++) {
        uint64_t node_off=i<24?VRS_DIAG_CATALOG_FIRST+(uint64_t)i*VRS_DIAG_NODE_BYTES:VRS_DIAG_ATTR_NODE21;
        uint64_t node_end=node_off+VRS_DIAG_NODE_BYTES;
        if(end<=node_off || off>=node_end)continue;
        struct vrs_node_witness *w=&vrs_node_witnesses[i];
        uint32_t page=(uint32_t)(node_off/VRS_PAGE),slot=store.pages[page];
        const uint8_t *overlay_node=NULL,*observed=NULL;
        uint64_t physical=0,expected=0;
        if(slot && slot<=store.overlay_used && slot<=store.overlay_capacity) {
            overlay_node=store.overlay+(size_t)(slot-1)*VRS_PAGE+node_off%VRS_PAGE;
            physical=((uint64_t)pmap_find_phys(kernel_pmap,(vm_offset_t)overlay_node)<<PAGE_SHIFT)
                +((vm_offset_t)overlay_node&PAGE_MASK);
            expected=STREAM_BASE+((config.packed_bytes+PAGE_MASK)&~(uint64_t)PAGE_MASK)
                +(uint64_t)(slot-1)*VRS_PAGE+node_off%VRS_PAGE;
        }
        uint32_t previous_crc=w->crc,previous_slot=w->slot;
        uint64_t previous_sequence=w->write_sequence;
        int match=-1;
        if(read) {
            if(off>node_off || end<node_end)continue;
            observed=(const uint8_t *)data+(size_t)(node_off-off);
            if(w->write_sequence)match=slot==w->slot && overlay_node &&
                vrs_diag_crc(observed,VRS_DIAG_NODE_BYTES)==w->crc;
        } else {
            observed=overlay_node;
            uint64_t begin=off>node_off?off:node_off,finish=end<node_end?end:node_end;
            match=overlay_node && !memcmp(overlay_node+(size_t)(begin-node_off),
                (const uint8_t *)data+(size_t)(begin-off),(size_t)(finish-begin));
            if(overlay_node) {
                w->write_sequence=r->sequence;w->slot=slot;
                w->crc=vrs_diag_crc(overlay_node,VRS_DIAG_NODE_BYTES);
            }
        }
        if(slot && (!overlay_node || physical!=expected))match=0;
        int bad=match==0;
        uint32_t *count=read?&w->reads:&w->writes;
        if(((*count>=2) && (!bad || w->mismatches>=4)) || r->sample_count==VRS_DIAG_SAMPLES)continue;
        ++*count;if(bad)++w->mismatches;
        struct vrs_node_sample *s=&r->samples[r->sample_count++];
        s->offset=node_off;s->node=i<24?20648+i:21;s->fileid=i<24?4:8;
        s->slot=slot;s->last_slot=previous_slot;s->write_sequence=previous_sequence;
        s->last_crc=previous_crc;s->match=match;s->physical=physical;s->expected_physical=expected;
        if(observed) {
            s->crc=vrs_diag_crc(observed,VRS_DIAG_NODE_BYTES);s->kind=(int8_t)observed[8];
            s->records=(uint32_t)observed[10]<<8|observed[11];
            s->zero=vrs_diag_zero(observed,VRS_DIAG_NODE_BYTES);
        }
    }
}
/* Emit after unlocking: diagnostics must not extend the disk copy's critical
 * section or recurse into it through a console consumer. Budgets limit each
 * node to two read/write samples and four extra mismatches. */
static void vrs_diag_emit(const struct vrs_io_report *r)
{
    if(r->emit_capacity || r->emit_error)
        printf("VERMEER COW: %s seq=%llu io=%c off=%llx bytes=%lu used=%u/%u pages vrs=%d errno=%d errors=%u\n",
            r->error==VRS_FULL?"FULL":(r->error?"ERROR":"USED"),r->sequence,
            r->read?'R':'W',r->offset,(unsigned long)r->bytes,r->used,r->capacity,
            r->error,r->error?io_error(r->error):0,r->errors);
    for(unsigned i=0;i<r->sample_count;i++) {
        const struct vrs_node_sample *s=&r->samples[i];
        printf("VERMEER COW: node-io v1 seq=%llu io=%c request=%llx/%lu fileid=%u node=%u off=%llx slot=%u last-write=%llu last-slot=%u crc=%08x last-crc=%08x kind=%d records=%u zero=%d match=%d pa=%llx expected-pa=%llx\n",
            r->sequence,r->read?'R':'W',r->offset,(unsigned long)r->bytes,s->fileid,s->node,
            s->offset,s->slot,s->write_sequence,s->last_slot,s->crc,s->last_crc,
            s->kind,s->records,s->zero,s->match,s->physical,s->expected_physical);
    }
}
int vermeer_packed_synchronize(int devid)
{
    if(!vermeer_packed_active(devid))return ENXIO;
    lck_mtx_lock(&stream_lock);
    /* vrs_write has already copied all bytes while holding this mutex.
     * Order that RAM copy and the page index before returning a flush. */
    __sync_synchronize();
    int report=!vrs_sync_reported;vrs_sync_reported=1;
    lck_mtx_unlock(&stream_lock);
    if(report)printf("VERMEER COW: volatile synchronize v1; copies complete before biodone\n");
    return 0;
}
int vermeer_packed_buf(struct buf *bp,uint64_t offset,void *data)
{
    struct vrs_io_report report;
    lck_mtx_lock(&stream_lock);
    int read=(buf_flags(bp)&B_READ)!=0;
    int error=read?vrs_read(&store,offset,data,buf_count(bp)):
        vrs_write(&store,offset,data,buf_count(bp));
    vrs_diag_locked(&report,offset,data,buf_count(bp),read,error);
    lck_mtx_unlock(&stream_lock);
    vrs_diag_emit(&report);
    return error?io_error(error):0;
}
int vermeer_packed_uio(struct uio *uio)
{
    uint8_t *temp=kalloc_data(VRS_PAGE,Z_WAITOK);
    if (!temp)return ENOMEM;
    int error=0;
    while (uio_resid(uio)>0 && (uint64_t)uio_offset(uio)<store.logical_bytes) {
        uint64_t off=(uint64_t)uio_offset(uio),remaining=store.logical_bytes-off;
        size_t n=(size_t)uio_resid(uio);if(n>VRS_PAGE)n=VRS_PAGE;if(n>remaining)n=(size_t)remaining;
        if (uio_rw(uio)==UIO_WRITE) {
            uio_t copy=uio_duplicate(uio);
            if (!copy) { error=ENOMEM;break; }
            error=uiomove((caddr_t)temp,(int)n,copy);uio_free(copy);if(error)break;
        }
        struct vrs_io_report report;
        lck_mtx_lock(&stream_lock);
        int result=uio_rw(uio)==UIO_READ?vrs_read(&store,off,temp,n):vrs_write(&store,off,temp,n);
        vrs_diag_locked(&report,off,temp,n,uio_rw(uio)==UIO_READ,result);
        lck_mtx_unlock(&stream_lock);
        vrs_diag_emit(&report);
        if (result) { error=io_error(result);break; }
        if (uio_rw(uio)==UIO_WRITE)uio_update(uio,(user_size_t)n);
        if (uio_rw(uio)==UIO_READ) { error=uiomove((caddr_t)temp,(int)n,uio);if(error)break; }
    }
    kfree_data(temp,VRS_PAGE);return error;
}
#endif
