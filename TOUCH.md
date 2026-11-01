# K70 physical touch candidate

This is an experimental native XNU input path, not a Linux module loader.
Hardware validation is pending. The existing Hello milestone did not include
physical input: GUI009 records zero touch calls and HID dispatches after the
reported swipe, with Setup/SpringBoard still in their main event loops.

The GUI profile alone includes `vermeer_touch.c`. `vermeer_touch=1` and the exact
`physical-model=Redmi K70` ADT property are required. The enabled K70 firmware DT
selects `xiaomi,vermeer-spi` on GENI SE4 at0xa90000, CS0, with GPIO44..47 for
qup1_se4_l0..3. GPIO24 is reset and GPIO25 interrupt. Clock101 is
GCC_QUPV3_WRAP1_S4_CLK: vote bit26 at GCC0x52008, branch0x184e4. Wrapper1 AHB
uses vote bits20/21, branches0x2327c/0x23280. Wrapper core2x/core additionally
use bits18/19, branches0x23294/0x23284, matching the UniXNU SM8550 board path.
TLMM register addressing includes
the Kalama0x100000 offset. The disabled I2C alternative is not used.

The worker waits for a permanently retained HID service, votes only those
five clocks, and checks core/SE4 readiness before accessing the sequencer. Following
MiCode's clock driver, an AHB clock in hardware-gated mode may report off while
idle; its vote permits bus accesses to wake it. A parked SE4 root can be force
requested without changing its rate, parent, DFS table or PLL. The additional
root-enable bit is restored on failure. Clock snapshots are retained for USB
queries without performing MMIO in the query callback.
It requires SPI firmware,32-bit FIFOs, an idle engine and reset already high.
Only the four observed SPI pins are configured; reset/interrupt GPIOs and PMIC
rails are not changed. Existing GCC root/DFS/PLL settings are retained; a large
internal serial divider is used for the initial transport.

There is no DMA, SMMU update, firmware/configuration upload or storage access.
Version PID9916 and checksums must match, followed by bounded IC-information
tables with8-byte header/point records and valid native axes. Android's
143999/319999 coordinates are virtual precision, not wire coordinates. Valid
native positions map to the1440x3200 physical panel; existing HID translation
maps that panel to the centered1125x2436 iOS canvas.

Header, contact count, unique finger IDs, type, coordinates and payload checksum
are validated before publishing any part of a frame. Every physical report is
now one complete HID collection, including active and released contacts. Failed
event or child-array allocation leaves the previous state intact and retries
the retained frame for up to one second. Duplicate stationary frames do not
generate repeated events. The only
controller data write clears the verified event status after a valid report.
Unknown request/stylus formats are not treated as fingers. A bus error retires
the transport; repeated malformed reports stop it and release held contacts.
Clock/pin/register state is restored only when the engine is idle. An engine
that does not quiesce keeps its resources until reboot and is never reused.

Host tests cover malformed/truncated packets, variable IC tables, native-axis
scaling, multi-contact release,1..1032-byte FIFO transfers and tails, impossible
FIFO counts, error/timeout retirement. These checks cannot establish rail state,
firmware compatibility, electrical communication, IRQ behavior or real touch.

## 2026-11-02 desktop/touch priority update

GPU acceleration and cellular integration are outside this candidate. The
centered 1125x2436 canvas keeps a contact's initial edge classification until
release, using the documented HID FromEdgeTip flag for touches originating
within 48 pixels of a canvas edge or in the physical black margins. A touch
that begins in the center does not become an edge touch merely by moving there.

SPI transfer deadlines scale with byte count (20 us polls, 16 kbit/s minimum
budget plus 10 ms overhead; at most 526 ms). The old fixed 50 ms budget could
expire while reading long IC information on the inherited clock divided by128.
Malformed or unsupported controller request/gesture events are not finger
events. They stop with a specific diagnostic instead of an endless ready loop;
no configuration upload or reset sequence is invented.

The actual included HID implementation is tested with failures at each parent,
child and child-array allocation, verifying no partial dispatch, lost release,
state advance on failure, leaks or duplicate stationary events. These software
checks do not replace a physical touch test.

USB still requires the exact supported SMMU register geometry, the unique SID40
route and an identity/bypass context. Only sCR0 fault-reporting enable bits1/2/4/5
may differ from the saved value; all translation/control bits remain checked.
The SMMU is never reprogrammed. This removes rejection solely for a diagnostic
interrupt setting; it cannot fix an actually translated or inaccessible route.

Protocol/register references (no binary firmware included):

- [MiCode GENI SPI](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/53ef33eacdc4f7ac737451e4cfc91963d59e3956/drivers/spi/spi-geni-qcom.c),
  [Kalama clocks](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/53ef33eacdc4f7ac737451e4cfc91963d59e3956/drivers/clk/qcom/gcc-kalama.c),
  [Kalama pinctrl](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/53ef33eacdc4f7ac737451e4cfc91963d59e3956/drivers/pinctrl/qcom/pinctrl-kalama.c).
- [Exact-compatible Xiaomi9916 reference](https://github.com/Air-Alex/kernel_xiaomi_sm8550-modules/tree/d92dbc093c08af885d8a808a170a715c1c024312/qcom/opensource/touch-drivers/goodix_berlin_driver_9916).
- [UniXNU SM8550 touch board](https://github.com/UniXNU/UniXNU-sheng/blob/7562a779ebdbdedc17c4c7274ba993acbbec8188/package/TouchBoard.h)
  and [GENI SPI](https://github.com/UniXNU/NativeGeniSpi/tree/b5399b5646c4d7415deec6d887fd51715a97a024).
