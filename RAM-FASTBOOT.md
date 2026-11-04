# K70 complete-root RAM Fastboot candidate

This route receives the complete compressed root **before XNU starts**. It is a
new boot candidate, not a report of successful physical USB, desktop or touch.

The handset's measured temporary download limit is 768 MiB. The existing packed
root is 5,884,403,712 bytes and its logical HFS volume is 13 GiB. Neither fits as
one temporary IMG. The root block driver already decompresses blocks on demand;
expanding the whole logical volume in a 12 GB handset is not the proposed route.

1. The host verifies the small image, its embedded ADT, the root metadata and
   the entire packed-root SHA256 before issuing any handset command.
2. One stock vermeer Fastboot handset temporarily boots the small image.
3. The shim checks live FDT RAM ownership, disabled MMU/cache, inherited clocks,
   DWC3 identity/device mode, SMMU geometry/control and unique SID40 identity route.
   It requires those gates; it does not change clocks, PMIC or SMMU mappings.
4. A fixed, bounded DMA pool in the future XNU allocator window serves USB2
   Fastboot, VID/PID 18d1:d00d, serial VERMEERRAM001. Microsoft OS descriptors
   advertise WinUSB and the Android interface GUID. Windows binding and physical
   enumeration remain hardware tests. No COM port is used.
5. The host checks loader identity and the compiled root size/hash, then uses
   `fastboot stage` in chunks of at most 64 MiB. Every chunk must be acknowledged
   with its exact cumulative byte count. No partition write command is issued.
6. `oem ram-boot` is accepted only after the exact root length and streaming SHA
   pass. INFO and OKAY must finish transmitting; then DWC3 must halt before the
   fixed DMA pool may be reused by XNU. A halt failure keeps the shim screen.
7. XNU uses `vermeer_ram_preloaded=1` with a matching ADT property and
   `vermeer_usb=0`. It maps the external root, independently hashes all packed
   bytes and validates index/HFS metadata before registering md0. Reads decompress
   blocks in RAM; writes use the bounded RAM overlay. Android storage is excluded.

The 2 GiB XNU allocator and the separate 6 GiB packed-root/overlay aperture are
retained. The live lower-8-GiB memory guard is required. The upper 1 GiB is excluded.
USB idle timeout, incorrect commands/data, unexpected configured reset or
disconnect, and failed hardware gates stop the session without a reboot loop.

`tools/send_ram_fastboot.py` defaults to local preflight. Only explicit `--boot`
issues the temporary boot and transfer. Logs distinguish loader SHA confirmation
from actual XNU handoff, root mount, Apple desktop and physical touch; the latter
must be observed on the handset. Successful host exit does not prove USB DMA
halt or XNU execution, which happen after the last ACK. The prior network-thread
startup stall is also not claimed fixed by this transport change.

Validation includes the C protocol/SHA vectors, full actual packed-root transfer
through the C core, >4-GiB offsets, actual USB descriptor parsing, mocked DWC3
event/ACK/halt sequencing, mocked actual XNU preload startup and host command,
identity, corruption and order checks. Hardware timing, enumeration, desktop and
touch cannot be established by these tests.
