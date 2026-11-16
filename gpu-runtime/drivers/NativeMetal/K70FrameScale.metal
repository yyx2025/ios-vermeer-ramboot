#include <metal_stdlib>
using namespace metal;
struct ScaleConstants {
    uint source_stride_words,target_stride_words,lx,ly,x0,y0,x1,y1,lw,lh,pw,ph;
};
/* Integer nearest-neighbour mapping matches vermeer_frame_scale.h exactly.
 * Source is an immutable clipped snapshot. Pixels are copied as packed BGRA
 * words; no float sampling, colour conversion or interpolation is involved. */
kernel void k70_frame_scale(device const uint *source [[buffer(0)]],
    device uint *target [[buffer(1)]],constant ScaleConstants &p [[buffer(2)]],
    uint2 local [[thread_position_in_grid]])
{
    const uint x=p.x0+local.x,y=p.y0+local.y;
    if(x>=p.x1 || y>=p.y1)return;
    const uint sx=(x*p.lw)/p.pw-p.lx,sy=(y*p.lh)/p.ph-p.ly;
    target[y*p.target_stride_words+x]=source[sy*p.source_stride_words+sx];
}
