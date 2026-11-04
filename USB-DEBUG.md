# K70 USB debug console

## 2026-11-03 polled complete-root startup candidate

The user reports that `Start-iOS-EarlyUSB.cmd` still does not produce a USB
device. No final handset register line or new capture is available yet, so the
hardware stopping point is not established. The previous build is therefore a
failed user-reported attempt, not a proven USB fix.

The RAM profile now owns USB polling in the BSD bootstrap caller, waits for
CDC configuration/DTR before mapping the 6 GiB root aperture, services USB
between 16 MiB mapping batches, then receives and verifies the complete root
before network initialization. It drains the COMMIT ACK before handing the
existing DMA allocation to the log worker. Early bytes before READY are rejected.
There is no second controller initialization or concurrent polling owner.

All endpoint/halt/reset waits use the hardware counter with AArch64 YIELD.
XNU's `delay()` can block a thread; its ARM spin fallback uses WFE. Those waits
are unsuitable for establishing USB when timer/event delivery is uncertain.
The new enumeration/receive pump never calls delay, WFE, tsleep or thread_block.
Other XNU allocation/locking and later OS startup still need a working kernel.

EP0 reset in DATA/STATUS terminates its active transfer before a STALL and
fresh SETUP; an already armed SETUP is retained. Reset after configuration
still stops the transfer rather than silently reusing in-flight RAM frames.
Reference: Linux v6.1 DWC3 gadget reset handler and EP0 restart.

Clocks, DWC identity, SMMU geometry/control, unique SID40 identity route, DMA
width/contiguity and live FDT RAM guards are retained. This change cannot repair
an unsupported PHY/SMMU state. Host tests and compilation are separate from
physical enumeration, root COMMIT, Apple desktop and touch verification.

## RAM startup ordering after the 2026-11-02 failed session

The user reported two successful fastboot downloads/boot commands followed by
no K70DBG004 enumeration, with the handset remaining at `ifnet_attach: Waiting
for all kernel threads created for interface ...`. The interface name was not
readable. This is a network startup wait; it does not establish a Windows
driver failure or identify the stalled thread. The previous RAM receiver ran
after network initialization, so it could not expose logs from this wait.

The RAM profile now sets up the checked receiver and starts CDC before
`dlil_init`, after the existing BSD memory/time initialization. The same guarded
RAM mapping, SMMU/clock predicates, checksums and single DMA owner apply. Later
root registration still waits for normal network initialization and complete,
verified root reception. Re-entering startup returns its saved result and does
not reset the controller or allocate a second receiver.

DLIL/interface startup keeps its original pending-thread predicates and locks.
Five-second timed sleeps add short `VERMEER NET` lines with the actual interface
name and pending count. A timeout never marks an unready interface as attached.
This change makes the pre-USB stopping point observable; it is not proof that
the underlying thread stall, handset USB or Apple desktop is fixed.

Experimental opt-in CDC ACM serial interface, integrated with the iOS profile.

2026-11-04 K70 RAM-stream test: the polled candidate from commit
`4d2cc2847d4ec1e59ec9e5c7ddfb08892d5ee66d` stopped with
`early USB receive setup failed: 6` before CDC enumeration. The host found no
matching serial/PnP device and received no COMMIT acknowledgment. The user had
already rebooted and recalled normal logs/waiting for the root, with no separate
USB error. ENXIO identifies an unsuccessful driver start, not the failed guard.
Earlier hardware checkpoints below are inherited history, not validation of
this RAM-stream candidate.

Startup refusals now retain the first reason in a bounded buffer and include it
in the BSD bootstrap panic. Clock/SMMU/route checks include the values they
examined; formerly silent mapping, allocation, DMA lookup/continuity and wrapper
failures have distinct messages. The original hardware gates remain in place.
`tests/usb_start_failure_test.py` exercises the actual initialization functions
with fake MMIO, including zero controller writes on pre-start refusals and
single-owner/no-retry behavior. This diagnostic change does not establish the
physical failure cause or working enumeration.

Hardware checkpoint2026-10-21: integrated004 (source `f0aec9c`,
Actions39540409885) enumerated with Windows Usbser as COM6, streamed early
kernel and Apple-service crash logs, and answered `status` through bulk OUT/IN.
No USB errors appeared during that capture. This does not establish iOS GUI,
panic-time transport or reconnect support after a physical disconnect.
The initial capture saved108,604bytes; a subsequent close/reopen test passed
`ping`, `help`, `status` and retained early-log `replay` on the same boot.

Boot with `vermeer_usb=1`. The default and diagnostic profiles do not start USB.
Windows uses its built-in Usbser driver for CDC ACM (class02/subclass02).
The laboratory VID/PID is1209:0001; synthetic serial `K70DBG004`.

The interface streams the most recent1MiB of console text, then live output.
Open the COM port with DTR asserted. Speed115200/8N1 is informational on USB.
`tools/capture-debug.ps1` finds this exact synthetic device and records bytes,
rotating at32MiB per file. Captures and device information stay outside Git.

Commands, each followed by a newline:

- `help`: command list
- `ping`: build-tag response
- `status`: event/byte counters and dropped console bytes
- `replay`: replay the retained RAM log
- `sample`: queue a read-only snapshot of the GUI sampler's boot-selected
  processes, or report unavailable if disabled/the40-snapshot limit is reached
- `frame` (GUI009): submitted/presented frame counts, vsync activity, copied
  bytes and missing/rejected surfaces; reports the current CPU copy backend
- `input` (GUI009): touch entry and HID dispatch counts; these are observations,
  not a touch injection command or proof of a physical controller driver

`-ObserveFrames` makes the capture tool query `frame`/`input` every ten seconds
while continuing to collect logs on the same serial connection. Both callbacks
read counters only; they do not take a display/HID lock or access hardware.
GUI009 validates these queries on the handset, including after the user reports
a frozen Hello picture. Timer ticks continue while submission/presentation
counts stay constant and input counts remain zero. Counter continuity does not
prove visible animation or working touch.

The next touch candidate extends `input` with an atomic physical-driver status:
stage1 waits for HID,2 clocks,3 sequencer firmware,4 controller identity,
5 ready,6 stopped. Report/checksum/transaction/emitted-event counts and the last
header distinguish controller failures from missing HID dispatch. Reading this
status never accesses MMIO or the HID lock. The physical SPI worker runs only
with the separate `vermeer_touch=1` boot option; the USB query cannot enable it.

The GUI007 sampler records saved user thread stacks and bounded dyld64 private
image paths/bases, allowing migration-plugin addresses to be symbolicated.
Work runs on a separate sampler thread. Requests coalesce and snapshots are at
least10seconds apart; the timer starts at90seconds, then every120seconds.
`sweetsample=-` disables it. Names use the kernel's16-character process name.
The host cannot supply addresses, task names or read sizes. Image enumeration
uses fault-safe reads, at most4096entries/64private rows and159path bytes per
row; a changing image list is marked best effort. The shared cache uses its
separately recorded slide. Stack lines are split into four-frame chunks so the
console formatter retains line boundaries. GUI007 and GUI008 pass manual USB
sampling and post-sample ping on the test handset, including GUI008 after the
user reports a frozen picture.

No shell, arbitrary memory access, storage operations or reboot commands are exposed.
This is log transport and bounded queries, not an interactive kernel debugger.
After a cable disconnect, configured bus reset, overflow or DMA/endpoint error,
the interface stops and retains all DMA memory. Reboot the temporary kernel to
reconnect. iOS continues independently; panic-time polling is not implemented.

Hardware guards require the SM8550 USB power/clocks already enabled by ABL,
DWC3/3.1 device mode, sufficient DMA address width, one identity SMMU route for
SID0x40, and contiguous memory in the independently tested2GiB high-RAM range.
The driver never changes clock, PHY/repeater, PMIC or SMMU configuration; only
DWC device registers and the Qualcomm wrapper's VBUS-valid override are set.
An unsupported hardware state skips USB and leaves the normal boot running.

Hardware enumeration and live capture must be verified separately from host
tests and compilation. A screen displaying this build's USB state may still be
needed for the first attempt.

Register/protocol references:
[Linux DWC3](https://github.com/torvalds/linux/tree/v6.1/drivers/usb/dwc3),
[SM8550 GCC](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-sm8550.c),
[Windows CDC ACM](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids).
