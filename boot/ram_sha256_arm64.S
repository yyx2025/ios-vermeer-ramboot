/* FIPS 180-4 SHA256 batches using ARMv8 SHA2 instructions.
 * Keep all round constants and digest state in registers across blocks.
 * Aligned state/input; caller-clobbered registers only. No stack spills,
 * absolute pointers, cache changes or peripheral access.
 */
.arch armv8-a+crypto
.text
.balign 16
.global rfb_sha256_block_arm64
.type rfb_sha256_block_arm64,%function
rfb_sha256_block_arm64:
    mov x2,#1
    b rfb_sha256_blocks_arm64
.size rfb_sha256_block_arm64,.-rfb_sha256_block_arm64
.balign 16
.global rfb_sha256_blocks_arm64
.type rfb_sha256_blocks_arm64,%function
rfb_sha256_blocks_arm64:
    cbz x2,.Lsha_done
    ldp q0,q1,[x0]
    adr x3,.Lsha_k
    ldp q16,q17,[x3]
    ldp q18,q19,[x3,#32]
    ldp q20,q21,[x3,#64]
    ldp q22,q23,[x3,#96]
    ldp q24,q25,[x3,#128]
    ldp q26,q27,[x3,#160]
    ldp q28,q29,[x3,#192]
    ldp q30,q31,[x3,#224]
.Lsha_blocks:
    ldp q2,q3,[x1]
    ldp q4,q5,[x1,#32]
    rev32 v2.16b,v2.16b
    rev32 v3.16b,v3.16b
    rev32 v4.16b,v4.16b
    rev32 v5.16b,v5.16b
    umov x3,v0.d[0]
    umov x4,v0.d[1]
    umov x5,v1.d[0]
    umov x6,v1.d[1]
.macro rounds msg,k
    add v7.4s,\k\().4s,\msg\().4s
    mov v6.16b,v0.16b
    sha256h q0,q1,v7.4s
    sha256h2 q1,q6,v7.4s
.endm
.macro group msg,next,third,fourth,k
    rounds \msg,\k
    sha256su0 \msg\().4s,\next\().4s
    sha256su1 \msg\().4s,\third\().4s,\fourth\().4s
.endm
    group v2,v3,v4,v5,v16
    group v3,v4,v5,v2,v17
    group v4,v5,v2,v3,v18
    group v5,v2,v3,v4,v19
    group v2,v3,v4,v5,v20
    group v3,v4,v5,v2,v21
    group v4,v5,v2,v3,v22
    group v5,v2,v3,v4,v23
    group v2,v3,v4,v5,v24
    group v3,v4,v5,v2,v25
    group v4,v5,v2,v3,v26
    group v5,v2,v3,v4,v27
    rounds v2,v28
    rounds v3,v29
    rounds v4,v30
    rounds v5,v31
    fmov d7,x3
    ins v7.d[1],x4
    add v0.4s,v0.4s,v7.4s
    fmov d7,x5
    ins v7.d[1],x6
    add v1.4s,v1.4s,v7.4s
    add x1,x1,#64
    subs x2,x2,#1
    b.ne .Lsha_blocks
    stp q0,q1,[x0]
.Lsha_done:
    ret
.size rfb_sha256_blocks_arm64,.-rfb_sha256_blocks_arm64
.section .rodata.ram_sha_k,"a"
.balign 16
.Lsha_k:
    .word 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5
    .word 0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
    .word 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3
    .word 0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
    .word 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc
    .word 0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
    .word 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7
    .word 0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
    .word 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13
    .word 0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
    .word 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3
    .word 0xd192e819,0xd6990624,0xf40e3585,0x106aa070
    .word 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5
    .word 0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
    .word 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208
    .word 0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
