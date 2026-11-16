/* SPDX-License-Identifier: MIT
 *
 * One line every 15 s with the kernel's own page accounting, emitted to the
 * panel console and mirrored on the USB CDC-ACM port.
 *
 * XNU owns a 2 GiB VM window; the RAM root uses a separate external
 * reserved range. These samples establish memory pressure, while panic and
 * process-exit records are required to identify the actual failure.
 *
 * The counters are the plain globals the VM already exports; reading them
 * without the page-queue lock can, at worst, mix two adjacent samples, which
 * is fine for a once-every-15-seconds trend line.  No snprintf here on
 * purpose: this unit only includes the osfmk headers the USB console already
 * proved, and formats its three numbers by hand.
 */
#include <mach/mach_types.h>
#include <mach/clock_types.h>
#include <mach/vm_param.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <mach/thread_act.h>

extern uint32_t vm_page_free_count;
extern uint32_t vm_page_free_target;
extern uint32_t vm_page_wire_count;
extern uint32_t vm_page_active_count;
extern uint32_t vm_page_inactive_count;
extern uint32_t vm_page_speculative_count;

extern void k70_usb_console_line(const char *text);

static volatile int k70_mem_thread_started;
static int k70_mem_wait_channel;

#define K70_MEM_MB(pages) ((unsigned)(((uint64_t)(pages) * PAGE_SIZE) >> 20))

static char *
k70_mem_put(char *out, const char *text)
{
	while (*text) {
		*out++ = *text++;
	}
	return out;
}

static char *
k70_mem_put_u32(char *out, unsigned value)
{
	char digits[11];
	unsigned int n = 0;

	do {
		digits[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value);
	while (n) {
		*out++ = digits[--n];
	}
	return out;
}

static void
k70_mem_thread(void *arg __unused, wait_result_t result __unused)
{
	for (;;) {
		char line[192];
		char *p = line;
		uint64_t deadline;
		unsigned free_mb = K70_MEM_MB(vm_page_free_count);
		unsigned wire_mb = K70_MEM_MB(vm_page_wire_count);
		unsigned inactive_mb = K70_MEM_MB(vm_page_inactive_count);

		printf("K70 MEM: free=%uMB target=%uMB wire=%uMB active=%uMB inactive=%uMB spec=%uMB\n",
		    free_mb, K70_MEM_MB(vm_page_free_target), wire_mb,
		    K70_MEM_MB(vm_page_active_count), inactive_mb,
		    K70_MEM_MB(vm_page_speculative_count));
		/* Keep the same 15-second cadence on CDC so a short respring loop has
		 * a usable memory record, including reclaimable active pages. */
		{
			p = k70_mem_put(p, "K70 MEM: free=");
			p = k70_mem_put_u32(p, free_mb);
			p = k70_mem_put(p, "MB target=");
			p = k70_mem_put_u32(p, K70_MEM_MB(vm_page_free_target));
			p = k70_mem_put(p, "MB wire=");
			p = k70_mem_put_u32(p, wire_mb);
			p = k70_mem_put(p, "MB active=");
			p = k70_mem_put_u32(p, K70_MEM_MB(vm_page_active_count));
			p = k70_mem_put(p, "MB inactive=");
			p = k70_mem_put_u32(p, inactive_mb);
			p = k70_mem_put(p, "MB spec=");
			p = k70_mem_put_u32(p, K70_MEM_MB(vm_page_speculative_count));
			p = k70_mem_put(p, "MB");
			*p = 0;
			k70_usb_console_line(line);
		}
		clock_interval_to_deadline(15, NSEC_PER_SEC, &deadline);
		(void)assert_wait_deadline(&k70_mem_wait_channel, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* Idempotent; never sleeps the caller. */
int
k70_mem_report_start(void)
{
	thread_t thread;
	kern_return_t result;

	if (__atomic_exchange_n(&k70_mem_thread_started, 1, __ATOMIC_ACQ_REL)) {
		return 0;
	}
	result = kernel_thread_start(k70_mem_thread, NULL, &thread);
	if (result != KERN_SUCCESS) {
		__atomic_store_n(&k70_mem_thread_started, 0, __ATOMIC_RELEASE);
		printf("K70 MEM: cannot create reporter thread: %d\n", result);
		return -1;
	}
	thread_deallocate(thread);
	return 1;
}
