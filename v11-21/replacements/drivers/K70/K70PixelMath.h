/* SPDX-License-Identifier: MIT */
#ifndef K70_PIXEL_MATH_H
#define K70_PIXEL_MATH_H
/* RGB10A8_2P_XR10 stores B/G/R in bits 0/10/20 and alpha in a second
 * plane. XR10 code 384 is zero, 894 is one. The display accepts BGRA8.
 * CA's colors are premultiplied: source-over must not multiply them again. */
static inline unsigned k70_xr10_byte(unsigned code) {
    if(code<=384)return 0;
    if(code>=894)return 255;
    return (code-384+1)/2;
}
static inline unsigned k70_over_byte(unsigned source,unsigned destination,unsigned alpha) {
    unsigned sum=source+(destination*(255-alpha)+127)/255;
    return sum>255?255:sum;
}
static inline unsigned k70_xr10_bgra(unsigned word,unsigned alpha,unsigned destination,bool base) {
    if(!alpha)return base?0xff000000u:destination;
    unsigned b=k70_xr10_byte(word&1023u),g=k70_xr10_byte((word>>10)&1023u),r=k70_xr10_byte((word>>20)&1023u);
    if(!base && alpha!=255) {
        b=k70_over_byte(b,destination&255u,alpha);
        g=k70_over_byte(g,(destination>>8)&255u,alpha);
        r=k70_over_byte(r,(destination>>16)&255u,alpha);
    }
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
#endif
