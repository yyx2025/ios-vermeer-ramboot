# K70 RAM root read consistency v5

The v4 hardware session transferred all 5,884,407,808 packed-root bytes and
verified the complete stored-RAM SHA256 before requesting XNU. The user
reports the same full-sha256-mismatch in XNU. This moves investigation past
the loader's stored-root check; it does not prove a specific corruption cause.

This version cleans previous-stage CPU data caches before the first store,
then uses by-VA maintenance on live-FDT-validated owned high RAM before its
first write. The final kernel/root ranges are synchronized to PoC before
the independent handoff samples and kernel jump. MMU/data-cache settings,
clock, SMMU and physical-storage policy stay as defined by the RAM profile.
The classic and CCIDX cache geometries are supported. See boot/CACHE-NOTICE.txt.

The kernel validates vermeer-ram-windows (VMRW0001): contiguous bounded
windows covering every packed byte, sealed to the same full root SHA256.
Each bounded read goes through the existing managed 1 MiB scratch buffer;
window and full-file hashes consume that same snapshot. A bad window gives
mapped-root-window-mismatch with window, offset and expected/actual hashes.
Every window must match and the independent full-file SHA must still match
before packed index, decoder, CRC, HFS and readiness checks proceed.

Reference: https://docs.kernel.org/arch/arm64/booting.html (cache, DMA and
reserved-memory handoff); https://asahilinux.org/docs/sw/m1n1-user-guide/
(USB kernel/initramfs development flow). Device-specific storage and MMIO
values from other ports are not imported.

Local checks exercise the actual window function, failure ordering and actual
ARM64 maintenance instructions/operand loop. QEMU cannot establish physical
cache consistency. Full kernel build, host preflight and hardware observations
are recorded separately. An explicit OEM success still precedes the later
DMA halt, final cache synchronization, handoff samples and kernel execution;
the host exit alone does not prove root mount, desktop or touch.
