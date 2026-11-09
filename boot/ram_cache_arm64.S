/* Copyright (c) 2011-2017, ARM Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * ARMv8 set/way data-cache maintenance before any store by this stage.
 * General registers only; x19-x30 and SP are preserved. MMU/cache stay off.
 * Cleaning must precede writing the root: cleaning old dirty lines afterwards
 * could overwrite the new DRAM contents. Handle both CCSIDR encodings.
 */
.text
.balign 16
.global ram_cache_prepare
.type ram_cache_prepare,%function
ram_cache_prepare:
    mrs x0, CurrentEL
    cmp x0, #4
    b.ne .Lcache_refused
    mrs x0, sctlr_el1
    mov x1, #5
    tst x0, x1
    b.ne .Lcache_refused
    mrs x8, id_aa64mmfr2_el1
    ubfx x8, x8, #20, #4
    cmp x8, #1
    b.hi .Lcache_refused
    dsb sy
    mrs x13, csselr_el1
    mrs x0, clidr_el1
    ubfx x3, x0, #24, #3
    lsl x3, x3, #1
    mov x10, #0
    cbz x3, .Lcache_done
.Lcache_level:
    add x2, x10, x10, lsr #1
    lsr x1, x0, x2
    and x1, x1, #7
    cmp x1, #2
    b.lo .Lcache_next
    cmp x1, #4
    b.hi .Lcache_restore_refused
    msr csselr_el1, x10
    isb
    mrs x1, ccsidr_el1
    and x2, x1, #7
    add x2, x2, #4
    cbz x8, .Lcache_classic
    ubfx x4, x1, #3, #21
    ubfx x7, x1, #32, #24
    b .Lcache_geometry
.Lcache_classic:
    ubfx x4, x1, #3, #10
    ubfx x7, x1, #13, #15
.Lcache_geometry:
    clz w5, w4
.Lcache_set:
    mov x9, x4
.Lcache_way:
    lsl x6, x9, x5
    orr x11, x10, x6
    lsl x6, x7, x2
    orr x11, x11, x6
    dc cisw, x11
    subs x9, x9, #1
    b.ge .Lcache_way
    subs x7, x7, #1
    b.ge .Lcache_set
    /* Finish inner writebacks before maintaining the next outer level. */
    dsb sy
.Lcache_next:
    add x10, x10, #2
    cmp x10, x3
    b.lo .Lcache_level
.Lcache_done:
    msr csselr_el1, x13
    dsb sy
    isb
    mov w0, #0
    ret
.Lcache_restore_refused:
    msr csselr_el1, x13
    dsb sy
    isb
.Lcache_refused:
    mov w0, #1
    ret
.size ram_cache_prepare,.-ram_cache_prepare

/* VA maintenance reaches architecturally coherent system caches too.
 * Call only on live-FDT-validated owned physical RAM with MMU/cache off.
 * Before the first writes it retires previous-stage dirty lines safely;
 * afterwards it synchronizes this stage's kernel/root data to PoC.
 */
.balign 16
.global ram_cache_sync_range
.type ram_cache_sync_range,%function
ram_cache_sync_range:
    mrs x2, CurrentEL
    cmp x2, #4
    b.ne .Lrange_refused
    mrs x2, sctlr_el1
    mov x3, #5
    tst x2, x3
    b.ne .Lrange_refused
    cmp x0, x1
    b.hi .Lrange_refused
    b.eq .Lrange_success
    mrs x2, ctr_el0
    ubfx x2, x2, #16, #4
    mov x3, #4
    lsl x2, x3, x2
    sub x3, x2, #1
    cmn x1, x3
    b.cs .Lrange_refused
    bic x0, x0, x3
    dsb sy
.Lrange_line:
    dc civac, x0
    add x0, x0, x2
    cmp x0, x1
    b.lo .Lrange_line
    dsb sy
    isb
.Lrange_success:
    mov w0, #0
    ret
.Lrange_refused:
    mov w0, #1
    ret
.size ram_cache_sync_range,.-ram_cache_sync_range
