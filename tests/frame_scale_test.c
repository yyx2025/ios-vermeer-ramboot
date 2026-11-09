#define VERMEER_HOST_TEST 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/vermeer/vermeer_frame_scale.h"
static int plan(struct vframe_scale *p,int sx,int sy,int dx,int dy,int w,int h,uint64_t bytes)
{
    return vframe_scale_plan(p,sx,sy,dx,dy,w,h,1125,2436,4512,16,bytes,
        1125,2436,1440,3200,5760,18432000);
}
int main(void)
{
    struct vframe_scale p;
    uint64_t len=16+4512ull*2436;
    assert(plan(&p,0,0,0,0,1125,2436,len));
    assert(p.x0==0 && p.y0==0 && p.x1==1440 && p.y1==3200 && p.src==16);
    for(unsigned y=0;y<3200;y++)for(unsigned x=0;x<1440;x++) {
        uint32_t lx=vframe_scale_x(&p,x),ly=vframe_scale_y(&p,y);
        assert(lx==x*1125u/1440 && ly==y*2436u/3200);
        assert(p.src+ly*4512ull+lx*4+4<=len);
    }
    struct vframe_scale saved=p;
    assert(!plan(&p,0,0,0,0,1125,2436,len-100));assert(!memcmp(&saved,&p,sizeof(p)));
    assert(!plan(&p,0,0,0,0,0,2436,len));
    assert(!plan(&p,0,0,INT32_MIN,0,100,100,len));
    /* Adjacent layers have no gap or overlap after scaling. */
    struct vframe_scale left,right;
    assert(plan(&left,0,0,0,0,563,2436,len));
    assert(plan(&right,563,0,563,0,562,2436,len));assert(left.x1==right.x0);
    uint32_t rng=4321;
    for(unsigned i=0;i<25000;i++) {
        rng=rng*1664525+1013904223;int sx=(int)(rng%1600)-250;
        rng=rng*1664525+1013904223;int sy=(int)(rng%3000)-250;
        rng=rng*1664525+1013904223;int dx=(int)(rng%1600)-250;
        rng=rng*1664525+1013904223;int dy=(int)(rng%3000)-250;
        rng=rng*1664525+1013904223;int w=(int)(rng%1600),h=(int)(rng%3000);
        if(!plan(&p,sx,sy,dx,dy,w,h,len))continue;
        for(unsigned y=p.y0;y<p.y1;y+=17)for(unsigned x=p.x0;x<p.x1;x+=17) {
            int lx=(int)(x*1125u/1440),ly=(int)(y*2436u/3200);
            int rx=sx+lx-dx,ry=sy+ly-dy;
            assert(rx>=0 && rx<1125 && ry>=0 && ry<2436);
            assert(lx>=dx && lx<dx+w && ly>=dy && ly<dy+h);
            uint64_t off=p.src+vframe_scale_y(&p,y)*4512ull+vframe_scale_x(&p,x)*4;
            assert(off==16+ry*4512ull+rx*4 && off+4<=len);
        }
    }
    puts("Full-panel pixels, adjacent layers, clipping, short buffers and signed geometry: PASS");
}
