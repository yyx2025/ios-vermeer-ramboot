/* Read live ABL FDT only. Candidate results describe fixed reservations,
 * not a hardware memory test or authorization to use a larger XNU window.
 */
#include "console.h"
#include "ram_guard.h"
#ifdef VERMEER_RAM_WRITE_RESTORE
#include "ram_probe.h"
static uint64_t probe_saved[RP_MAX_SAMPLES];
static uint64_t probe_read(void *ctx, uint64_t address)
{ (void)ctx; return *(volatile const uint64_t *)(uintptr_t)address; }
static void probe_write(void *ctx, uint64_t address, uint64_t value)
{ (void)ctx; *(volatile uint64_t *)(uintptr_t)address = value; }
static void probe_barrier(void *ctx)
{ (void)ctx; __asm__ volatile("dsb sy" ::: "memory"); }
#endif

void ram_map_dump(uintptr_t dtb);
void ram_map_dump(uintptr_t dtb)
{
    if (dtb < 0xb5d00000 || dtb >= 0xb5f00000 || (dtb & 7)) {
        con_str("MAP", "FDT pointer outside bounded ABL region");
        return;
    }
    struct rg_layout layout;
    const uint8_t *tree = (const uint8_t *)dtb;
    size_t available = 0xb5f00000 - dtb;
    int result = ram_guard_collect(tree, available, HIGH_RAM_LO, HIGH_RAM_HI, &layout);
    con_hex("GUARD2G", (unsigned)result);
    if (result) return; /* Do not present partially parsed banks as valid. */
    for (unsigned i = 0; i < layout.count; i++) {
        con_hex("BANK", i);
        con_hex("BASE", layout.bank[i].base);
        con_hex("SIZE", layout.bank[i].size);
    }
    con_str("STATIC", "fixed FDT checks only; zero = accepted");
    con_hex("BASE", HIGH_RAM_LO);
    con_hex("CHECK4G", ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + UINT64_C(0x100000000)));
    con_hex("CHECK6G", ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + UINT64_C(0x180000000)));
    con_hex("CHECK8G", ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + UINT64_C(0x200000000)));
    con_hex("CHECK10G", ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + UINT64_C(0x280000000)));
#ifdef VERMEER_RAM_WRITE_RESTORE
    /* Keep the upper GiB clear, including Android runtime-reserved regions.
     * The live ABL fixed-reservation guard remains mandatory as well.
     */
    uint64_t bytes = RP_MAX_BYTES, sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    con_hex("SCTLR", sctlr);
    if (sctlr & 5) { con_str("RESULT", "REFUSED: MMU/data cache enabled"); return; }
    int write_guard = ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + bytes);
    con_hex("WRITE8G", (unsigned)write_guard);
    if (write_guard) return;
    extern char _head[], _end[];
    if ((uintptr_t)_head < HIGH_RAM_LO + bytes && (uintptr_t)_end > HIGH_RAM_LO) {
        con_str("RESULT", "REFUSED: shim overlaps RAM test"); return;
    }
    uintptr_t saved_start = (uintptr_t)probe_saved;
    if (saved_start < HIGH_RAM_LO + bytes && saved_start + sizeof(probe_saved) > HIGH_RAM_LO) {
        con_str("RESULT", "REFUSED: saved words overlap RAM test"); return;
    }
    con_str("WRITE", "65536 sampled words; patterns then restore");
    /* Volatile member stores prevent absolute callback pointers in a pooled
     * read-only initializer; this shim is relocated by ABL without fixups.
     */
    volatile struct rp_io io;
    io.context = NULL; io.read = probe_read;
    io.write = probe_write; io.barrier = probe_barrier;
    struct rp_result test_result;
    int status = rp_run(&io, HIGH_RAM_LO, bytes, RP_MAX_SAMPLES, probe_saved, &test_result);
    con_hex("STATUS", (unsigned)status);
    con_hex("RESTORED", (unsigned)test_result.restored);
    if (status) {
        con_hex("PHASE", test_result.phase); con_hex("ADDR", test_result.address);
        con_hex("WANT", test_result.expected); con_hex("GOT", test_result.actual);
        con_str("RESULT", "SPARSE WRITE FAIL");
    } else {
        con_str("RESULT", "SPARSE WRITE ALIAS PASS; RESTORED");
    }
#endif
#ifdef VERMEER_RAM_READ_ONLY
    uint64_t bytes = UINT64_C(0x240000000); /* 9 GiB, inferred from real Android DDR entries. */
    int read_guard = ram_guard(tree, available, HIGH_RAM_LO, HIGH_RAM_LO + bytes);
    con_hex("READ9G", (unsigned)read_guard);
    if (read_guard) return;
    con_str("READONLY", "sparse physical reads; no RAM writes");
    const unsigned samples = 32768;
    for (unsigned i = 0; i < samples; i++) {
        uint64_t offset = ((bytes - 8) * i / (samples - 1)) & ~UINT64_C(7);
        /* Volatile aligned accesses cannot be optimized away. No values are
         * logged and no candidate RAM location is modified or initialized.
         */
        (void)*(volatile const uint64_t *)(uintptr_t)(HIGH_RAM_LO + offset);
    }
    con_hex("READS", samples);
    con_str("RESULT", "SPARSE READ PASS; write/alias test pending");
#endif
}
