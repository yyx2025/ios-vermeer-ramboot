/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <sys/systm.h>
#include <kern/clock.h>
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
    uint64_t nanos=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&nanos);
    char framed[600];
    const int n=snprintf(framed,sizeof framed,"[K70 t=%llums] %s",(unsigned long long)(nanos/1000000),line);
    if(n<=0)return;
    IOLog("%s",framed);
    (void)qcom_usb_debug_enqueue(framed,(size_t)n<sizeof framed?(size_t)n:sizeof framed-1);
}
