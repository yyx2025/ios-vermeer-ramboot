/* SPDX-License-Identifier: MIT
 *
 * K70 consumer for QcomXnuUsbDebug: bring up the SM8550 DWC3 controller as a
 * polled CDC-ACM serial port so the host PC gets a COM port from this kernel.
 *
 * Why this exists
 * ---------------
 * `oem ram-boot` hands control to a kernel that contains no USB device stack
 * at all (no IOUSBDeviceController, no AppleUSBDevice, no gadget driver), so
 * nothing on the port ever answers the host: the device simply is not there.
 * The QUD module drives the same controller the bootloader used, in polled
 * mode, with only the boot CPU servicing hardware.  That yields a plain byte
 * stream to the host with no XNU console patch, no IRQ and no ITS.
 *
 * Design constraints inherited from the module (see its docs/boundaries.md):
 *  - hardware is serviced on the boot CPU only, so the worker is bound there;
 *  - the controller must be inherited in device mode with its power domain up
 *    (ABL configures it for fastboot); if that state is not qualified the
 *    module refuses instead of guessing, and this consumer only reports it;
 *  - static DMA arenas must be physically contiguous, which the module checks
 *    through AT S1E1R on our behalf.
 */
#include <mach/mach_types.h>
#include <mach/clock_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
#include "QcomXnuUsbDebug.h"

static volatile int k70_usb_thread_started;
static volatile int k70_usb_active;
static int k70_usb_wait_channel;

/* One line on the panel console, so a boot that cannot reach USB still tells
 * us which gate refused it.  printf() is the kernel console path the rest of
 * this port already uses for its VERMEER/SWEET lines. */
static void
k70_usb_describe(const char *tag)
{
	struct qcom_usb_debug_status s;

	qcom_usb_debug_get_status(&s);
	printf("K70 USB: %s phase=%u error=%u qcom=%u domain=%u stage=%u bank=%u slot=%u io=%u\n",
	    tag, s.phase, s.error, s.qcom_result, s.domain_result, s.domain_stage,
	    s.domain_bank, s.domain_slot, s.io_failed);
	printf("K70 USB: dma_pa=0x%llx iova=0x%llx width=%u hwparams0=0x%x vbus=%u/%u phy=%u/%u\n",
	    s.dma_pa, s.dma_iova, s.dma_address_width, s.hwparams0,
	    s.vbus_owned, s.vbus_saved, s.phy_owned, s.phy_saved);
}

void
k70_usb_console_write(const char *text, unsigned int length)
{
	if (!k70_usb_active || !text || !length) {
		return;
	}
	(void)qcom_usb_debug_write(text, length);
}

void
k70_usb_console_line(const char *text)
{
	char out[220];
	size_t i = 0;

	if (!text) {
		return;
	}
	while (text[i] && i < sizeof(out) - 2) {
		out[i] = text[i];
		i++;
	}
	out[i++] = '\n';
	k70_usb_console_write(out, (unsigned int)i);
}

int
k70_usb_console_active(void)
{
	return k70_usb_active;
}

/* The module calls this through a weak_import when the controller reaches
 * QCOM_USB_DEBUG_ACTIVE, so the panel gets the confirmation even before the
 * host has attached its terminal. */
/* The module's control path can ask the consumer to hand the port back to the
 * bootloader for a normal reset. This port has no qualified fastboot reset
 * conduit and the framed control channel is not enabled, so the honest answer
 * is a refusal: reset requests must never silently reboot the machine. */
int
boot_reset_bootloader(char *why, unsigned int length)
{
	static const char reason[] = "K70: no fastboot reset conduit";

	if (why && length) {
		unsigned int i = 0;
		while (reason[i] && i + 1 < length) {
			why[i] = reason[i];
			i++;
		}
		why[i] = 0;
	}
	return -1;
}

void
boot_progress_persist(const char *text, uint32_t colour)
{
	(void)colour;
	if (!text) {
		return;
	}
	k70_usb_console_line(text);
}

static void
k70_usb_thread(void *arg __unused, wait_result_t result __unused)
{
	processor_t boot = cpu_to_processor(master_cpu);
	unsigned int attempt;
	uint64_t polls = 0;
	int active;

	if (boot == PROCESSOR_NULL) {
		goto done;
	}
	/* Hardware must be serviced from the boot CPU: the module's owner guard
	 * rejects every other CPU and the controller state is not per-CPU. */
	thread_bind(boot);
	thread_block(THREAD_CONTINUE_NULL);
	if (cpu_number() != master_cpu) {
		goto done;
	}

	/* v11.31: this line is the binary marker that the MMIO windows are
	 * mapped before the owner guard (v11.28 panicked in zalloc with DAIF
	 * masked by the guard).  If you see it, the fix is in this kernel. */
	printf("K70 USB: MMIO mapped before owner guard\n");
	printf("K70 USB: controller takeover starting (DWC3 polled CDC-ACM)\n");
	/* The inherited state is whatever ABL left behind, and the first seconds
	 * after the jump are not necessarily the settled state: the PHY PLL can
	 * still be locking and the power domain can still be ramping.  v11.27 made
	 * exactly one attempt and, if it lost that race, the port stayed dead for
	 * the whole session.  Retry with a growing pause instead; each gate in the
	 * module is a pure read-back test, so a retry costs a few microseconds and
	 * can never corrupt the controller. */
	active = 0;
	for (attempt = 0; attempt < 6; attempt++) {
		struct qcom_usb_debug_status s;
		uint64_t deadline;

		if (attempt) {
			clock_interval_to_deadline(1u << (attempt - 1), NSEC_PER_SEC,
			    &deadline);
			(void)assert_wait_deadline(&k70_usb_wait_channel,
			    THREAD_UNINT, deadline);
			thread_block(THREAD_CONTINUE_NULL);
		}
		active = qcom_usb_debug_init();
		qcom_usb_debug_get_status(&s);
		printf("K70 USB: attempt %u %s phase=%u error=%u qcom=%u domain=%u\n",
		    attempt, active ? "active" : "refused", s.phase, s.error,
		    s.qcom_result, s.domain_result);
		if (active) {
			break;
		}
		/* One refusal is final: GCTL[13:12] is not 2, so the controller is not
		 * in device mode -- ABL either switched it to host or shut it down.
		 * No amount of waiting brings it back; that needs our own power and
		 * clock bring-up, which is a separate stage. */
		if (s.error == 8 /* CORE_NOT_DEVICE */) {
			break;
		}
	}
	k70_usb_describe(active ? "active" : "refused");
	if (!active) {
		printf("K70 USB: not running; host will keep seeing no device\n");
		goto done;
	}
	k70_usb_active = 1;
	k70_usb_console_line("K70 USB console attached; host should enumerate a CDC-ACM port");

	for (;;) {
		struct qcom_usb_debug_status s;
		uint64_t deadline;

		qcom_usb_debug_get_status(&s);
		if (s.phase != QCOM_USB_DEBUG_ACTIVE) {
			k70_usb_describe("stopped");
			break;
		}
		qcom_usb_debug_poll();
		/* 5 ms matches the module's documented worker cadence: fast enough
		 * for bulk-IN windows at high speed, cheap enough between frames. */
		clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&k70_usb_wait_channel, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
		/* One line a minute, so a boot log read the next morning still shows
		 * whether the port was alive the whole time. */
		if ((++polls % 12000u) == 0) {
			k70_usb_describe("alive");
		}
	}
	k70_usb_active = 0;

done:
	thread_bind(PROCESSOR_NULL);
	thread_terminate(current_thread());
}

/* Idempotent. Safe from any IOKit start path; the worker itself decides
 * whether the inherited controller state is usable. */
int
k70_usb_console_start(void)
{
	thread_t thread;
	kern_return_t result;

	if (__atomic_exchange_n(&k70_usb_thread_started, 1, __ATOMIC_ACQ_REL)) {
		return 0;
	}
	result = kernel_thread_start(k70_usb_thread, NULL, &thread);
	if (result != KERN_SUCCESS) {
		__atomic_store_n(&k70_usb_thread_started, 0, __ATOMIC_RELEASE);
		printf("K70 USB: cannot create worker thread: %d\n", result);
		return -1;
	}
	thread_deallocate(thread);
	return 1;
}
