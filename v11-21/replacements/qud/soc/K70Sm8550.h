/* K70 board policy. Register facts: stock Kalama/vermeer DT,
 * ssusb@a600000, iommus SID0x40, IOVA pool0x90000000, dis_u2_susphy.
 * Supply, repeater, parent clocks and device role must be inherited.
 * Reset and control commands are disabled for this initial log test. */
#include "Sm8550.h"
#undef QUD_SOC_NAME
#define QUD_SOC_NAME "k70-sm8550-inherited"
#undef QUD_USB_PID
#define QUD_USB_PID 0x0001u
#undef QUD_USB_SERIAL
#define QUD_USB_SERIAL "K70XNU001"
#undef QUD_USB_PRODUCT
#define QUD_USB_PRODUCT "K70 XNU Boot Log"
#undef QUD_PSCI_SMC_RESET
#define QUD_PSCI_SMC_RESET 0

#define QUD_K70_PANIC_RESET 1
