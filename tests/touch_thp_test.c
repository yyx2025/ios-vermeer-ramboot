#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/vermeer/vermeer_touch_thp.h"
static struct vtt_work ws;
int main(void)
{
    struct vt_frame old={0},out={0};uint8_t data[4096]={0};
    assert(vtt_shape(18,40,96));assert(!vtt_shape(255,255,0));assert(!vtt_shape(3,40,0));
    assert(!vtt_shape(18,40,UINT32_MAX));assert(!vtt_shape(18,40,3000));
    data[0]=0x80;unsigned off=104,n=off+18*40*2;
    data[off+2*(9*40+20)+1]=4; /* A 1024-unit peak at the center. */
    for(unsigned cut=0;cut<n;cut++)assert(!vtt_decode(&ws,data,cut,18,40,96));
    assert(vtt_decode(&ws,data,n,18,40,96));
    assert(vtt_process(&ws,18,40,200,0,&old,&out));
    assert(ws.peaks==1 && out.point[0].active && out.point[0].x==760 && out.point[0].y==1640);
    assert(!old.point[0].active); /* Proposed state remains uncommitted. */
    old=out;memset(data+off,0,n-off);data[off+2*(9*40+21)+1]=4;
    assert(vtt_decode(&ws,data,n,18,40,96));assert(vtt_process(&ws,18,40,200,0,&old,&out));
    assert(out.point[0].active && out.point[0].y==1720 && !out.point[1].active);
    assert(vtt_process(&ws,18,40,200,6,&old,&out));
    assert(out.point[0].active && out.point[0].x==1439-760 && out.point[0].y==3199-1720);
    memset(&old,0,sizeof(old));ws.m[3*40+3]=32767;ws.m[15*40+36]=-32768;
    assert(vtt_process(&ws,18,40,200,0,&old,&out));assert(ws.peaks==3);
    for(unsigned i=0;i<VT_CONTACTS;i++)assert(out.point[i].x<1440 && out.point[i].y<3200);
    old=out;memset(ws.m,0,sizeof(ws.m));assert(vtt_process(&ws,18,40,200,0,&old,&out));
    for(unsigned i=0;i<VT_CONTACTS;i++)assert(!out.point[i].active);
    struct vt_frame saved=out;
    assert(!vtt_process(&ws,18,40,0,0,&old,&out));assert(!memcmp(&out,&saved,sizeof(out)));
    assert(!vtt_process(&ws,18,40,200,8,&old,&out));
    data[0]=0;assert(!vtt_decode(&ws,data,n,18,40,96));
    puts("THP frame bounds, centroid, mirrors, signed peaks, stable slots and release: PASS");
}
