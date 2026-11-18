/* SPDX-License-Identifier: MIT */
#ifndef K70_PIXEL_CONTRACT_H
#define K70_PIXEL_CONTRACT_H
#include <stdint.h>
#include "K70PixelMath.h"
enum {K70_PIXEL_BGRA=0x42475241u,K70_PIXEL_B3A8=0x62336138u};
struct K70PixelPlane {
    uint64_t offset,bytes;
    uint32_t stride,width,height,bpe,element_width,element_height;
};
struct K70PixelLayout {
    uint64_t bytes,offset;
    uint32_t format,width,height,stride,bpe,element_width,element_height,plane_count;
    K70PixelPlane plane[2];
};
static inline bool k70_pixel_span(uint64_t offset,uint64_t declared,uint32_t stride,
    uint32_t width,uint32_t height,uint32_t bpe,uint64_t bytes,uint64_t *end) {
    if(!width || !height || width>16384 || height>16384 || !bpe || bpe>4 ||
       !stride || uint64_t(width)*bpe>stride || offset>bytes || declared>bytes-offset)return false;
    const uint64_t span=uint64_t(height-1)*stride+uint64_t(width)*bpe;
    if(span>declared || span>bytes-offset)return false;
    if(end)*end=offset+span;
    return true;
}
static inline bool k70_pixel_layout_valid(const K70PixelLayout &p) {
    if(!p.bytes || p.bytes>(256u<<20) || !p.width || !p.height || p.width>16384 || p.height>16384 ||
       p.bpe!=4 || p.element_width!=1 || p.element_height!=1 || (p.stride&3))return false;
    if(p.format==K70_PIXEL_BGRA) {
        return !p.plane_count && p.offset<=p.bytes &&
            k70_pixel_span(p.offset,p.bytes-p.offset,p.stride,p.width,p.height,4,p.bytes,nullptr);
    }
    if(p.format!=K70_PIXEL_B3A8 || p.plane_count!=2)return false;
    uint64_t colorEnd=0,alphaEnd=0;
    for(unsigned i=0;i<2;++i) {
        const auto &q=p.plane[i];
        if(q.width!=p.width || q.height!=p.height || q.bpe!=(i?1u:4u) ||
           q.element_width!=1 || q.element_height!=1 || ((q.offset|q.stride)&15u) ||
           !k70_pixel_span(q.offset,q.bytes,q.stride,q.width,q.height,q.bpe,p.bytes,i?&alphaEnd:&colorEnd))return false;
    }
    /* Keep the exact layout the Metal runtime sees; no rounded or overlapping planes. */
    return p.plane[1].offset>=colorEnd && p.plane[0].offset<colorEnd && alphaEnd<=p.bytes;
}
static inline uint32_t k70_pixel_color_stride(const K70PixelLayout &p)
{return p.format==K70_PIXEL_B3A8?p.plane[0].stride:p.stride;}
static inline uint64_t k70_pixel_color_offset(const K70PixelLayout &p)
{return p.format==K70_PIXEL_B3A8?p.plane[0].offset:p.offset;}
static inline unsigned k70_pixel_word(const uint8_t *p)
{return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24);}
#endif
