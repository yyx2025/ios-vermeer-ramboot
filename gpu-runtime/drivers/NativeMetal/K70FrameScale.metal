#include <metal_stdlib>
using namespace metal;
#include "K70PixelMath.h"
struct ScaleConstants {
    uint source_stride_words,target_stride_words,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;
    uint format,alpha_offset,alpha_stride,previous_count;
    uint previous_clip[12],reserved[4];
};
/* Integer geometry matches the kernel clip. v3 explicitly converts b3a8's
 * separate XR10 color/alpha snapshots to BGRA8, preserving premultiplication.
 * A pixel's first coverage in this frame starts over black; subsequent
 * coverage uses source-over. The union's exterior retains the old frame. */
kernel void k70_frame_scale_v3(device const uint *source [[buffer(0)]],
    device uint *target [[buffer(1)]],constant ScaleConstants &p [[buffer(2)]],
    uint2 local [[thread_position_in_grid]])
{
    const uint x=p.x0+local.x,y=p.y0+local.y;
    if(x>=p.x1 || y>=p.y1)return;
    const uint sx=(x*p.lw)/p.pw-p.lx,sy=(y*p.lh)/p.ph-p.ly;
    const uint at=y*p.target_stride_words+x,word=source[sy*p.source_stride_words+sx];
    if(p.format==0x42475241u)target[at]=word;
    else if(p.format==0x62336138u) {
        const device uchar *bytes=reinterpret_cast<const device uchar *>(source);
        const uint alpha=bytes[p.alpha_offset+sy*p.alpha_stride+sx];
        bool base=true;
        for(uint i=0;i<p.previous_count;++i) {
            const uint n=i*4;
            if(x>=p.previous_clip[n] && y>=p.previous_clip[n+1] &&
               x<p.previous_clip[n+2] && y<p.previous_clip[n+3]){base=false;break;}
        }
        target[at]=k70_xr10_bgra(word,alpha,target[at],base);
    }
}
