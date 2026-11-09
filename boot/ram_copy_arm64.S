/* General-register copy for aligned, disjoint RAM. No cache or DMA changes. */
.text
.balign 16
.global rfb_copy64_arm64
.type rfb_copy64_arm64,%function
rfb_copy64_arm64:
    cbz x2,.Lcopy_done
.Lcopy_blocks:
    ldp x3,x4,[x1]
    ldp x5,x6,[x1,#16]
    ldp x7,x8,[x1,#32]
    ldp x9,x10,[x1,#48]
    stp x3,x4,[x0]
    stp x5,x6,[x0,#16]
    stp x7,x8,[x0,#32]
    stp x9,x10,[x0,#48]
    add x0,x0,#64
    add x1,x1,#64
    subs x2,x2,#1
    b.ne .Lcopy_blocks
.Lcopy_done:
    ret
.size rfb_copy64_arm64,.-rfb_copy64_arm64
