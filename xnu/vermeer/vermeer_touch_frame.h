/* One physical report -> one iOS digitizer collection. Pure value helpers;
 * allocation/publication failures must not commit the proposed next state. */
#ifndef VERMEER_TOUCH_FRAME_H
#define VERMEER_TOUCH_FRAME_H
#include <stdint.h>
#define VT_CONTACTS 10
struct vt_point { uint16_t x,y;uint8_t active; };
struct vt_frame { struct vt_point point[VT_CONTACTS]; };
enum { VTH_TOUCH=1,VTH_POSITION=2,VTH_IDENTITY=4,VTH_EDGE=8 };
struct vth_contact { int32_t x,y;unsigned down,edge; };
struct vth_state { struct vth_contact point[VT_CONTACTS]; };
struct vth_plan {
    struct vth_state next;
    unsigned include[VT_CONTACTS],mask[VT_CONTACTS],count,any;
    int32_t x,y;
};
static inline int vth_prepare(const struct vth_state *old,const struct vt_frame *frame,struct vth_plan *out)
{
    if(!old || !frame || !out)return -1;
#ifdef __cplusplus
    struct vth_plan p={};
#else
    struct vth_plan p={0};
#endif
    unsigned changed=0,primary=0;
    for(unsigned i=0;i<VT_CONTACTS;++i) {
        const struct vt_point *f=&frame->point[i];
        const struct vth_contact *before=&old->point[i];
        struct vth_contact *after=&p.next.point[i];
        if(f->active>1 || (f->active && (f->x>=1440 || f->y>=3200)))return -1;
        *after=*before;after->down=f->active;
        if(f->active) {
            int32_t x=(int32_t)f->x-157,y=(int32_t)f->y-382;
            if(x<0)x=0;
            if(x>1124)x=1124;
            if(y<0)y=0;
            if(y>2435)y=2435;
            /* Edge origin belongs to the centered visible canvas, including
             * a finger entering from the physical panel's black margins.
             * Keep it throughout contact lifetime; moving from the center
             * to an edge must not retroactively become an edge gesture. */
            if(!before->down)after->edge=x<48 || x>=1125-48 || y<48 || y>=2436-48;
            after->x=(int32_t)(((int64_t)x<<16)/1125);
            after->y=(int32_t)(((int64_t)y<<16)/2436);
        }
        if(before->down!=after->down){p.mask[i]|=VTH_TOUCH;changed=1;}
        if(after->down && (!before->down || before->x!=after->x || before->y!=after->y)) {
            p.mask[i]|=VTH_POSITION;changed=1;
        }
        if(!before->down && after->down)p.mask[i]|=VTH_IDENTITY;
        if(before->down || after->down) {
            p.include[i]=1;++p.count;
            if(after->edge)p.mask[i]|=VTH_EDGE;
            if(!primary || (after->down && !p.any)){p.x=after->x;p.y=after->y;primary=1;}
        }
        p.any|=after->down;
    }
    *out=p;return changed?1:0;
}
#endif
