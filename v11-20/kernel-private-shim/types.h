/*
 * v11.20 shim: <osfmk/string.h> switches to a bare #include <types.h> when
 * MACH_KERNEL_PRIVATE is defined, which the real XNU kernel build gets from a
 * directory on its include path. The retained build tree only exports
 * sys/types.h, so forward to it instead of pulling in a host header.
 */
#ifndef _K70_KERNEL_PRIVATE_TYPES_SHIM
#define _K70_KERNEL_PRIVATE_TYPES_SHIM 1
#include <sys/types.h>
#endif
