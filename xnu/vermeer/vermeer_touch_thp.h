/* GT9916S THP difference frames, based on K70 r33s touch implementation.
 * Bounds and input validation added; callers commit only after complete HID dispatch.
 */
#ifndef VERMEER_TOUCH_THP_H
#define VERMEER_TOUCH_THP_H
#include "vermeer_goodix_info.h"
#define VT_NODES_MAX 1500u
struct vtt_work { int16_t m[VT_NODES_MAX];uint8_t used[VT_NODES_MAX];unsigned peaks;int max,max_t,max_r; };
static inline int vtt_shape(unsigned t,unsigned r,unsigned skip)
{
    return t>=4 && r>=4 && t<=255 && r<=255 && t*r<=VT_NODES_MAX &&
        skip<=4000 && 8+t*r*2<=4000-skip;
}
static inline int vtt_decode(struct vtt_work *ws,const uint8_t *data,unsigned n,
    unsigned t,unsigned r,unsigned skip)
{
    if(!ws || !data || !vtt_shape(t,r,skip) || n<skip+8+t*r*2 || !(data[0]&0x80))return 0;
    for(unsigned i=0;i<t*r;i++)ws->m[i]=(int16_t)vt_le16(data+skip+8+2*i);
    return 1;
}
static inline int vtt_process(struct vtt_work *ws,unsigned T,unsigned R,unsigned threshold,unsigned map,
    const struct vt_frame *old,struct vt_frame *out)
{
    if(!ws || !old || !out || !vtt_shape(T,R,0) || threshold<1 || threshold>32768 || map>7)return 0;
    unsigned nodes=T*R;
    int thr=(int)threshold;
    struct vt_frame frame={0};
    struct {int x,y;} pk[5];unsigned np=0;
    for(unsigned i=0;i<nodes;i++)ws->used[i]=0;
    ws->max=0;
    for(unsigned k=0;k<5;k++) {
        int best=0;unsigned bi=0;
        for(unsigned i=0;i<nodes;i++) {
            int v=ws->m[i];if(v<0)v=-v;
            if(!ws->used[i] && v>best){best=v;bi=i;}
        }
        if(!k){ws->max=best;ws->max_t=(int)(bi/R);ws->max_r=(int)(bi%R);}
        if(best<thr)break;
        int pt=(int)(bi/R),pr=(int)(bi%R);
        int64_t sw=0,st=0,sr=0;
        for(int dt=-1;dt<=1;dt++)for(int dr=-1;dr<=1;dr++) {
            int t=pt+dt,r=pr+dr;
            if(t<0 || r<0 || t>=(int)T || r>=(int)R)continue;
            int v=ws->m[t*(int)R+r];if(v<0)v=-v;
            int w=v-thr/2;if(w<=0)continue;
            sw+=w;st+=(int64_t)w*(2*t+1);sr+=(int64_t)w*(2*r+1);
        }
        for(int dt=-2;dt<=2;dt++)for(int dr=-2;dr<=2;dr++) {
            int t=pt+dt,r=pr+dr;
            if(t>=0 && r>=0 && t<(int)T && r<(int)R)ws->used[t*(int)R+r]=1;
        }
        if(!sw)continue;
        /* centre positions in half-node units over [0, 2N): the long matrix axis is the screen's long (3200) axis */
        int64_t ct=st,cr=sr;
        int t_is_y=(T>=R)^((map&1u)!=0);
        int64_t cx=t_is_y?cr:ct,cy=t_is_y?ct:cr;
        int64_t nx=2L*(t_is_y?R:T),ny=2L*(t_is_y?T:R);
        int x=(int)(cx*1440/(nx*sw)),y=(int)(cy*3200/(ny*sw));
        if(map&2u)x=1439-x;
        if(map&4u)y=3199-y;
        if(x<0)x=0;if(x>1439)x=1439;if(y<0)y=0;if(y>3199)y=3199;
        pk[np].x=x;pk[np].y=y;np++;
    }
    ws->peaks=np;
    /* Keep a finger's slot across frames: nearest previous contact within 300 px, else the first free slot. */
    unsigned claimed=0;
    for(unsigned k=0;k<np;k++) {
        int slot=-1;int64_t bestd=300L*300L;
        for(unsigned s=0;s<VT_CONTACTS;s++) {
            if(!old->point[s].active || (claimed&(1u<<s)))continue;
            int64_t dx=(int64_t)old->point[s].x-pk[k].x,dy=(int64_t)old->point[s].y-pk[k].y;
            if(dx*dx+dy*dy<bestd){bestd=dx*dx+dy*dy;slot=(int)s;}
        }
        if(slot<0)for(unsigned s=0;s<VT_CONTACTS;s++)
            if(!old->point[s].active && !(claimed&(1u<<s))){slot=(int)s;break;}
        if(slot<0)continue;
        claimed|=1u<<slot;
        frame.point[slot].active=1;frame.point[slot].x=(uint16_t)pk[k].x;frame.point[slot].y=(uint16_t)pk[k].y;
    }
    *out=frame;
    return 1;
}
#endif
