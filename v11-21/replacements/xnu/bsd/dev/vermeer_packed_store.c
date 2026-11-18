#include "vermeer_packed_store.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32; }
static uint64_t align4096(uint64_t n)
{ return (n+4095)&~UINT64_C(4095); }
static uint32_t crc32_block(const uint8_t *p, size_t n)
{
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<n;i++) {
        crc^=p[i];
        for (unsigned j=0;j<8;j++) crc=(crc>>1)^(UINT32_C(0xedb88320)&(0u-(crc&1u)));
    }
    return ~crc;
}
static int overlaps(const void *a,size_t n,const void *b,size_t m)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if (n>UINTPTR_MAX-x || m>UINTPTR_MAX-y) return 1;
    return x<y+m && y<x+n;
}
int vrs_open(struct vrs_store *s,const void *packed,size_t length,void *cache,
             uint32_t *pages,size_t entries,void *overlay,uint32_t capacity,
             vrs_sha_check sha,vrs_decode decode,void *context)
{
    if (!s) return VRS_INVALID;
    memset(s,0,sizeof(*s));
    if (!packed || length<VRS_ALIGN || !cache || !pages || !overlay || !sha || !decode ||
        !capacity || (uint64_t)capacity*VRS_PAGE>SIZE_MAX) return VRS_INVALID;
    const uint8_t *p=packed;
    if (memcmp(p,"VMRAM001",8) || le32(p+8)!=VRS_FORMAT_VERSION || le32(p+12)!=VRS_ALIGN ||
        le32(p+24)!=VRS_BLOCK || le64(p+32)!=VRS_ALIGN || le64(p+48)!=length)
        return VRS_INVALID;
    uint64_t logical=le64(p+16);uint32_t blocks=le32(p+28);
    if (!logical || logical>VRS_MAX_LOGICAL || blocks!=(logical+VRS_BLOCK-1)/VRS_BLOCK ||
        entries<(logical+VRS_PAGE-1)/VRS_PAGE || entries>SIZE_MAX/sizeof(*pages)) return VRS_INVALID;
    uint64_t data=align4096(VRS_ALIGN+(uint64_t)blocks*32);
    if (data>length || data!=le64(p+40)) return VRS_INVALID;
    if (overlaps(packed,length,cache,VRS_BLOCK) || overlaps(packed,length,pages,entries*4) ||
        overlaps(packed,length,overlay,(size_t)capacity*VRS_PAGE) ||
        overlaps(cache,VRS_BLOCK,pages,entries*4) || overlaps(cache,VRS_BLOCK,overlay,(size_t)capacity*VRS_PAGE) ||
        overlaps(pages,entries*4,overlay,(size_t)capacity*VRS_PAGE)) return VRS_INVALID;
    if (!sha(context,p+VRS_ALIGN,(size_t)blocks*32,p+88)) return VRS_CORRUPT;
    for (size_t i=120;i<VRS_ALIGN;i++) if (p[i]) return VRS_INVALID;
    uint64_t cursor=data;
    for (uint32_t i=0;i<blocks;i++) {
        const uint8_t *e=p+VRS_ALIGN+(size_t)i*32;
        uint64_t off=le64(e);uint32_t n=le32(e+8),decoded=le32(e+12),codec=le32(e+16);
        uint64_t remain=logical-(uint64_t)i*VRS_BLOCK;
        uint32_t expected=remain<VRS_BLOCK?(uint32_t)remain:VRS_BLOCK;
        if (decoded!=expected || le64(e+24)) return VRS_INVALID;
        if (!codec) { if (off || n) return VRS_INVALID; }
        else {
            if (codec>VRS_CODEC_ZSTD || !n || n>decoded || off!=cursor || off>length ||
                n>length-off || (codec==1 && n!=decoded)) return VRS_INVALID;
            cursor=align4096(off+n);
            if (cursor>length) return VRS_INVALID;
        }
    }
    if (cursor!=length) return VRS_INVALID;
    memset(pages,0,entries*sizeof(*pages));
    s->packed=p;s->packed_bytes=length;s->logical_bytes=logical;s->data_offset=data;
    s->blocks=blocks;s->cache=cache;s->pages=pages;s->page_entries=entries;
    s->overlay=overlay;s->overlay_capacity=capacity;s->decode=decode;s->codec_context=context;
    s->ready=1;return VRS_OK;
}
static int load_block(struct vrs_store *s,uint32_t block)
{
    if (s->cache_valid && s->cache_block==block) return VRS_OK;
    s->cache_valid=0;
    const uint8_t *e=s->packed+VRS_ALIGN+(size_t)block*32;
    uint32_t n=le32(e+8),decoded=le32(e+12),codec=le32(e+16);
    if (!codec) memset(s->cache,0,decoded);
    else if (codec==1) memcpy(s->cache,s->packed+le64(e),n);
    else if (!s->decode(s->codec_context,codec,s->cache,decoded,s->packed+le64(e),n)) return VRS_CORRUPT;
    if (crc32_block(s->cache,decoded)!=le32(e+20)) return VRS_CORRUPT;
    s->cache_block=block;s->cache_valid=1;return VRS_OK;
}
static int bounds(const struct vrs_store *s,uint64_t off,size_t n)
{ return !s || !s->ready || off>s->logical_bytes || n>s->logical_bytes-off; }
static int private_buffer(const struct vrs_store *s,const void *p,size_t n)
{
    return n && (overlaps(p,n,s->packed,s->packed_bytes) || overlaps(p,n,s->cache,VRS_BLOCK) ||
        overlaps(p,n,s->pages,s->page_entries*sizeof(*s->pages)) ||
        overlaps(p,n,s->overlay,(size_t)s->overlay_capacity*VRS_PAGE) || overlaps(p,n,s,sizeof(*s)));
}
int vrs_read(struct vrs_store *s,uint64_t off,void *output,size_t n)
{
    if (bounds(s,off,n) || (!output && n) || private_buffer(s,output,n)) return VRS_INVALID;
    uint8_t *out=output;
    while (n) {
        uint32_t page=(uint32_t)(off/VRS_PAGE);size_t within=(size_t)(off%VRS_PAGE);
        size_t count=VRS_PAGE-within;if (count>n) count=n;
        if (s->pages[page]) {
            memcpy(out,s->overlay+(size_t)(s->pages[page]-1)*VRS_PAGE+within,count);
        } else {
            int error=load_block(s,(uint32_t)(off/VRS_BLOCK));if (error) return error;
            memcpy(out,s->cache+off%VRS_BLOCK,count);
        }
        off+=count;out+=count;n-=count;
    }
    return VRS_OK;
}
int vrs_write(struct vrs_store *s,uint64_t off,const void *input,size_t n)
{
    if (bounds(s,off,n) || (!input && n) || private_buffer(s,input,n)) return VRS_INVALID;
    if (!n) return VRS_OK;
    uint32_t first=(uint32_t)(off/VRS_PAGE),last=(uint32_t)((off+n-1)/VRS_PAGE),missing=0;
    for (uint32_t page=first;page<=last;page++) if (!s->pages[page]) missing++;
    if (missing>s->overlay_capacity-s->overlay_used) return VRS_FULL;
    /* Validate original bytes for every newly copied page before any write. */
    for (uint32_t page=first;page<=last;page++) if (!s->pages[page]) {
        int error=load_block(s,page/(VRS_BLOCK/VRS_PAGE));if (error) return error;
    }
    const uint8_t *src=input;
    while (n) {
        uint32_t page=(uint32_t)(off/VRS_PAGE);size_t within=(size_t)(off%VRS_PAGE);
        if (!s->pages[page]) {
            int error=load_block(s,page/(VRS_BLOCK/VRS_PAGE));if (error) return error;
            uint64_t start=(uint64_t)page*VRS_PAGE,remaining=s->logical_bytes-start;
            size_t original=remaining<VRS_PAGE?(size_t)remaining:VRS_PAGE;
            uint32_t slot=s->overlay_used++;
            memcpy(s->overlay+(size_t)slot*VRS_PAGE,s->cache+start%VRS_BLOCK,original);
            if (original<VRS_PAGE) memset(s->overlay+(size_t)slot*VRS_PAGE+original,0,VRS_PAGE-original);
            s->pages[page]=slot+1;
        }
        size_t count=VRS_PAGE-within;if (count>n) count=n;
        memcpy(s->overlay+(size_t)(s->pages[page]-1)*VRS_PAGE+within,src,count);
        off+=count;src+=count;n-=count;
    }
    return VRS_OK;
}
