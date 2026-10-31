/* Read live ABL FDT only. Candidate results describe fixed reservations,
 * not a hardware memory test or authorization to use a larger XNU window.
 */
#include "console.h"
#include "ram_guard.h"

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
}
