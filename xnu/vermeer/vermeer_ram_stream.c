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
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <libkern/crypto/sha2.h>
#include "vermeer_packed_store.h"
#include "vermeer_ram_receive.h"
#include "lzfse/lzfse_internal.h"

#define STREAM_BASE UINT64_C(0x940000000)
#define STREAM_BYTES UINT64_C(0x180000000)
#define STREAM_OVERLAY_MAX UINT64_C(0x20000000)
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
LCK_GRP_DECLARE(stream_group,"vermeer-ram-root");
LCK_MTX_DECLARE(stream_lock,&stream_group);
extern dev_t mdevadd(int,uint64_t,unsigned int,int);
/* The ARM pmap_map prototype is in the Mach-private pmap header, which is
 * omitted from BSD's exported headers. Match the XNU-8796 implementation. */
extern vm_map_address_t pmap_map(vm_map_address_t,vm_offset_t,vm_offset_t,vm_prot_t,unsigned int);
extern void vermeer_usb_debug_start(void);
extern int vermeer_usb_running(void);
extern void vermeer_usb_set_ram_receiver(int (*)(const void *,unsigned));
int vermeer_ram_stream_boot(void);
int vermeer_packed_active(int);
int vermeer_packed_buf(struct buf *,uint64_t,void *);
int vermeer_packed_uio(struct uio *);

static int index_sha(void *ctx,const void *data,size_t n,const uint8_t *expected)
{
    (void)ctx;SHA256_CTX hash;uint8_t value[32];
    SHA256_Init(&hash);SHA256_Update(&hash,data,n);SHA256_Final(value,&hash);
    return !memcmp(value,expected,sizeof(value));
}
static int decode_block(void *ctx,void *dst,size_t n,const void *src,size_t stored)
{
    (void)ctx;memset(&decoder,0,sizeof(decoder));
    decoder.src=decoder.src_begin=src;decoder.src_end=decoder.src+stored;
    decoder.dst=decoder.dst_begin=dst;decoder.dst_end=decoder.dst+n;
    int result=lzfse_decode(&decoder);
    return result==LZFSE_STATUS_OK && decoder.end_of_stream &&
        (size_t)(decoder.dst-decoder.dst_begin)==n && decoder.src==decoder.src_end;
}
static int receive_begin(void *ctx,uint64_t bytes,const uint8_t *sha)
{
    (void)ctx;
    if (bytes!=config.packed_bytes || memcmp(sha,config.sha256,32)) return EINVAL;
    SHA256_Init(&transfer_hash);
    printf("VERMEER RAM: receiving complete packed root, %llu bytes; volatile only\n",bytes);
    return 0;
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
    SHA256_Final(hash,&transfer_hash);
    if (memcmp(hash,config.sha256,32)) return EIO;
    uint64_t overlay_off=(config.packed_bytes+PAGE_MASK)&~(uint64_t)PAGE_MASK;
    int error=vrs_open(&store,(void *)area,(size_t)config.packed_bytes,(void *)cache,
        (uint32_t *)page_index,VRS_MAX_LOGICAL/VRS_PAGE,(void *)(area+overlay_off),
        (uint32_t)(config.overlay_bytes/VRS_PAGE),index_sha,decode_block,NULL);
    if (error || vrs_read(&store,1024,hfs,sizeof(hfs))) return EIO;
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
    int result=vrx_feed(&receive,data,n);
    return result<0?-1:(receive.complete?1:0);
}

int vermeer_ram_stream_boot(void)
{
    int enabled=0;
    if (!PE_parse_boot_argn("vermeer_ram_stream",&enabled,sizeof(enabled)) || enabled!=1)return 0;
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
    /* This aperture is outside the retained 2GiB XNU physical allocator.
     * The shim supplies this ADT property only after the live 8GiB FDT guard.
     * Use normal coherent RAM attributes, never ml_io_map's device attributes
     * or the small early-I/O submap, for multi-GiB compressed bytes.
     */
    if (kmem_alloc(kernel_map,&area,STREAM_BYTES,KMA_DATA|KMA_PAGEABLE|KMA_PERMANENT,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS ||
        kmem_alloc(kernel_map,&cache,VRS_BLOCK,KMA_DATA|KMA_ZERO|KMA_PERMANENT,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS ||
        kmem_alloc(kernel_map,&page_index,(VRS_MAX_LOGICAL/VRS_PAGE)*sizeof(uint32_t),
        KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
    if (pmap_map(area,STREAM_BASE,STREAM_BASE+STREAM_BYTES,VM_PROT_READ|VM_PROT_WRITE,
        VM_WIMG_COPYBACK)!=area+STREAM_BYTES)return EIO;
    printf("VERMEER RAM: external RAM mapped, base=%llx bytes=%llx; XNU allocator remains 2GiB\n",
        config.base,config.bytes);
    struct vrx_ops ops={NULL,receive_begin,receive_data,receive_finish,receive_reply};
    vrx_init(&receive,&ops);
    vermeer_usb_set_ram_receiver(receiver_bytes);vermeer_usb_debug_start();
    if (!vermeer_usb_running())return ENXIO;
    printf("VERMEER RAM: READY VMRX0001; waiting for checked complete root; no storage writes\n");
    unsigned idle=0;uint64_t previous=0;
    while (!ready && !receive_failed && idle<10) {
        (void)tsleep(&sleep_channel,PZERO,"ramroot",60*hz);
        if (!vermeer_usb_running())return EIO;
        uint64_t received=receive_progress;
        if (received!=previous) { previous=received;idle=0; }
        else idle++;
    }
    if (!ready || receive_failed)return EIO;
    if (mdevadd(0,(uint64_t)cache>>12,(unsigned)(store.logical_bytes>>12),0)==NODEV)return ENXIO;
    printf("VERMEER RAM: compressed md0 registered; RAM writes limited to %llu bytes\n",config.overlay_bytes);
    return 0;
}
int vermeer_packed_active(int devid) { return devid==0 && ready; }
static int io_error(int error)
{ return error==VRS_FULL?ENOSPC:(error==VRS_INVALID?EINVAL:EIO); }
int vermeer_packed_buf(struct buf *bp,uint64_t offset,void *data)
{
    lck_mtx_lock(&stream_lock);
    int error=(buf_flags(bp)&B_READ)?vrs_read(&store,offset,data,buf_count(bp)):
        vrs_write(&store,offset,data,buf_count(bp));
    lck_mtx_unlock(&stream_lock);
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
        lck_mtx_lock(&stream_lock);
        int result=uio_rw(uio)==UIO_READ?vrs_read(&store,off,temp,n):vrs_write(&store,off,temp,n);
        lck_mtx_unlock(&stream_lock);
        if (result) { error=io_error(result);break; }
        if (uio_rw(uio)==UIO_WRITE)uio_update(uio,(user_size_t)n);
        if (uio_rw(uio)==UIO_READ) { error=uiomove((caddr_t)temp,(int)n,uio);if(error)break; }
    }
    kfree_data(temp,VRS_PAGE);return error;
}
#endif
