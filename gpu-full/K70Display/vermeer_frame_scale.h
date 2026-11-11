/* Whole-panel BGRA presentation. Clip in the established UIKit canvas before
 * scaling; every destination pixel belongs to exactly one logical pixel.
 * Source extents remain validated by vframe_clip. No allocation or GPU claim.
 */
#ifndef VERMEER_FRAME_SCALE_H
#define VERMEER_FRAME_SCALE_H
#include "vermeer_frame_policy.h"
struct vframe_scale {
    uint64_t src;
    uint32_t lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;
};
static inline int vframe_scale_plan(struct vframe_scale *out,
    int64_t sx,int64_t sy,int64_t dx,int64_t dy,int64_t w,int64_t h,
    uint32_t sw,uint32_t sh,uint32_t stride,uint64_t offset,uint64_t length,
    uint32_t lw,uint32_t lh,uint32_t pw,uint32_t ph,uint32_t pstride,uint64_t plen)
{
    struct vframe_copy copy;
    /* This path is only for the bounded known K70 framebuffer reservation. */
    if(!out || lw!=1125 || lh!=2436 || pw!=1440 || ph!=3200 || pstride!=5760 ||
       plen<(uint64_t)pstride*ph ||
       !vframe_clip(&copy,sx,sy,dx,dy,w,h,sw,sh,stride,offset,length,
           lw,lh,lw*4,(uint64_t)lw*4*lh,0,0))return 0;
    struct vframe_scale p;
    p.src=copy.src;p.lx=(uint32_t)(copy.dst%(lw*4))/4;
    p.ly=(uint32_t)(copy.dst/(lw*4));p.lw=lw;p.lh=lh;p.pw=pw;p.ph=ph;
    p.x0=(uint32_t)(((uint64_t)p.lx*pw+lw-1)/lw);
    p.x1=(uint32_t)(((uint64_t)(p.lx+copy.bytes/4)*pw+lw-1)/lw);
    p.y0=(uint32_t)(((uint64_t)p.ly*ph+lh-1)/lh);
    p.y1=(uint32_t)(((uint64_t)(p.ly+copy.rows)*ph+lh-1)/lh);
    if(p.x0>=p.x1 || p.y0>=p.y1)return 0;
    *out=p;return 1;
}
static inline uint32_t vframe_scale_x(const struct vframe_scale *p,uint32_t x)
{return (uint32_t)((uint64_t)x*p->lw/p->pw)-p->lx;}
static inline uint32_t vframe_scale_y(const struct vframe_scale *p,uint32_t y)
{return (uint32_t)((uint64_t)y*p->lh/p->ph)-p->ly;}
#endif
