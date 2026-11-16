/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <sys/systm.h>
#include "QcomXnuUsbDebug.h"

/* Preserve startup diagnostics even before CDC enumeration. Enqueue only:
 * display callers can hold locks, so USB hardware remains on its worker. */
static inline void K70BootLog(const char *format, ...) __attribute__((format(printf,1,2)));
static inline void K70BootLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args,format);
    const int length=vsnprintf(line,sizeof line,format,args);
    va_end(args);
    if(length<=0)return;
    const size_t count=(size_t)length<sizeof line?(size_t)length:sizeof line-1;
    IOLog("%s",line);
    (void)qcom_usb_debug_enqueue(line,count);
}
