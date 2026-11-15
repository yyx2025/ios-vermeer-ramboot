/*
 * v11.20 shim for the kernel-private compile of osfmk/arm64.
 *
 * With MACH_KERNEL_PRIVATE defined, the exported <osfmk/string.h> switches from
 * <sys/types.h> to a bare <types.h>. The retained build tree only exports the
 * BSD headers, so forward to the kernel's own libsa/types.h (guard
 * _MACH_TYPES_H_), which is what that include expects: it supplies size_t,
 * uint_t, caddr_t and friends without dragging in the BSD clock_t definition
 * that conflicts with kern/kern_types.h.
 */
#ifndef _K70_KERNEL_PRIVATE_TYPES_SHIM
#define _K70_KERNEL_PRIVATE_TYPES_SHIM 1
#include <libsa/types.h>
#endif
