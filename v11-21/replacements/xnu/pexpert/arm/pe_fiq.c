/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 */

#include <stdint.h>
#include <mach/boolean.h>
#include <libkern/section_keywords.h>
#include <arm64/proc_reg.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>


#if HAS_GIC_V3
/*
 * ml_io_map() is declared in <osfmk/arm/machine_routines.h> only under
 * PEXPERT_KERNEL_PRIVATE || MACH_KERNEL_PRIVATE.  The real pexpert build
 * passes one of them; the SWEET relink compiles this single unit with the
 * generic flags, so declare the one entry point it needs instead of
 * changing visibility for the whole file.
 */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_len);

#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

#if defined(SWEET)
/* Phone default; QEMU virt wires the architected virtual timer to INTID 27. */
SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;
/*
 * "sweetgic=0" leaves the per-core SGI frame alone (see the wake-up block in
 * pe_init_fiq).  Everything else -- ICC_SRE/PMR/IGRPEN1 and GICD_CTLR -- still
 * runs, so this is a usable escape hatch when a redistributor cannot be woken:
 * the boot continues, only the virtual-timer PPI and the SGI stay disabled.
 */
static uint32_t sweet_gic_ppi = 1;
#endif

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

#if defined(SWEET)
/*
 * Fault-tolerant device access.
 *
 * sleh.c (also rebuilt in this kernel) exports these: when a probe is active
 * and the access takes a synchronous external abort, the data-abort handler
 * records FAR/ESR and resumes at the following instruction instead of
 * panicking.  That turns "guess the GICR layout and lose the boot" into
 * "read the layout out of the hardware", which is what this file needs.
 */
extern volatile int sweet_mmio_probe_active;
extern volatile unsigned long long sweet_mmio_probe_far;
extern volatile unsigned long long sweet_mmio_probe_esr;

static int
sweet_probe_read32(vm_offset_t addr, uint32_t *value)
{
	sweet_mmio_probe_far = 0;
	sweet_mmio_probe_esr = 0;
	sweet_mmio_probe_active = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t v = *((volatile uint32_t *) addr);
	__asm__ volatile("dsb sy" ::: "memory");
	int faulted = (sweet_mmio_probe_active == 0);
	sweet_mmio_probe_active = 0;
	if (faulted) {
		*value = 0;
	} else {
		*value = v;
	}
	return faulted;
}

static int
sweet_probe_write32(vm_offset_t addr, uint32_t value)
{
	sweet_mmio_probe_far = 0;
	sweet_mmio_probe_esr = 0;
	sweet_mmio_probe_active = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	*((volatile uint32_t *) addr) = value;
	__asm__ volatile("dsb sy" ::: "memory");
	int faulted = (sweet_mmio_probe_active == 0);
	sweet_mmio_probe_active = 0;
	return faulted;
}

/* Offsets inside a redistributor frame (frame-relative, per GICv3). */
#define SWEET_GICR_CTLR          0x0000
#define SWEET_GICR_SGI_IGROUPR0  0x0080
#define SWEET_GICR_SGI_ISENABLER 0x0100
#define SWEET_GICR_SGI_ICENABLER 0x0180
#define SWEET_GICR_SGI_PRIORITY  0x0400

static vm_offset_t
sweet_gicr_page(vm_offset_t frame, vm_offset_t page)
{
	return frame + page;
}
#endif /* SWEET */

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
#if defined(SWEET)
		/*
		 * Probe log: on vermeer the secondaries fault inside this loop and
		 * XNU has no case for the resulting ESR class, so the panic message
		 * is useless ("Undefined kernel instruction").  These lines name the
		 * exact frame that was being read and, for every successfully read
		 * frame, its TYPER, so one photo of the boot log is enough.
		 */
		kprintf("SWEET GIC: probe cpu=%u offset=0x%lx base=0x%lx\n",
		    ml_get_cpu_number_local(), (unsigned long)offset, (unsigned long)gicr_base);
#endif
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

#if defined(SWEET)
		kprintf("SWEET GIC: typer cpu=%u offset=0x%lx typer=0x%llx aff=0x%x want=0x%x\n",
		    ml_get_cpu_number_local(), (unsigned long)offset,
		    (unsigned long long)gicr_typer, aff_value, phys_id);
#endif

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

#if defined(SWEET)
		/* Read only on the boot CPU, before the GIC and RO-late lockdown. */
		void const *timer_prop;
		unsigned int timer_size;
		if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess) {
			if (timer_size != sizeof(uint32_t)) {
				panic("%s: invalid virtual-timer-intid size %u", __func__, timer_size);
			}
			uint32_t intid = *(const uint32_t *)timer_prop;
			if (intid < 16 || intid >= 32) {
				panic("%s: virtual timer INTID %u is not a PPI", __func__, intid);
			}
			sweet_vtimer_intid = intid;
		}
		kprintf("SWEET GIC: virtual timer INTID %u\n", sweet_vtimer_intid);
#endif

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

#if defined(SWEET)
		/*
		 * The DT reg entries are relative to /arm-io ranges, so the absolute
		 * addresses are soc_base + prop.  Print them (not the mapped VAs, which
		 * are whatever io_map() handed out) so one photo of the boot log shows
		 * which physical window the GIC is being driven at.
		 */
		uint32_t sweet_ppi_arg = 1;
		if (PE_parse_boot_argn("sweetgic", &sweet_ppi_arg, sizeof(sweet_ppi_arg))) {
			sweet_gic_ppi = sweet_ppi_arg ? 1 : 0;
		}
		kprintf("SWEET GIC: soc=0x%lx gicd_pa=0x%lx gicr_pa=0x%lx gicr_size=0x%lx ppi=%u\n",
		    (unsigned long)soc_base_phys,
		    (unsigned long)(soc_base_phys + gicd_base_prop),
		    (unsigned long)(soc_base_phys + gicr_base_prop),
		    (unsigned long)gicr_size_prop, sweet_gic_ppi);
#endif

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
#if defined(SWEET)
	/*
	 * v11.25: discover the redistributor, do not compute it.
	 *
	 * Three hardware facts forced this rewrite:
	 *
	 *  - The vendor DTB declares redistributor-stride = 0x40000 on kalama.
	 *    GICR_PE_SIZE is 0x20000, and the ADT carries no stride property at
	 *    all, so "frame = cpu * 0x20000" is wrong for every secondary.
	 *  - A cold-boot redistributor answers WAKER = 0x6
	 *    (ProcessorSleep | ChildrenAsleep).  Clearing ProcessorSleep does
	 *    make ChildrenAsleep drop, i.e. the architectural wake-up sequence
	 *    completes ... and the very first 32-bit write to frame + 0x10080
	 *    (GICR_IGROUPR0) still returns ESR 0x96000010, a synchronous
	 *    external abort.  That is the v11.23/v11.24 panic on cpu 0.
	 *  - Therefore the SGI page offset inside the frame must not be assumed
	 *    to be 0x10000 either.
	 *
	 * So both offsets are located by probing, and every probe is guarded by
	 * the fault-tolerant accessor above: a bus error costs a log line
	 * instead of the boot.  The SGI page is then identified by writing
	 * GICR_ISENABLER0 and reading it back -- a page whose enable bits do
	 * not stick is not a GIC SGI page and is left alone.
	 */
	unsigned int sweet_cpu = ml_get_cpu_number_local();
	uint32_t want_aff = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") &
	    (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK));
	vm_offset_t rd_off = 0;
	uint64_t rd_typer = 0;
	unsigned int rd_found = 0, rd_faults = 0, rd_probes = 0;
	vm_offset_t sgi_off = 0;
	unsigned int sgi_found = 0, sgi_writable = 0;
	uint32_t waker_before = 0, waker_after = 0, sgi_enables = 0;
	unsigned int wake_ok = 0;

	if (!sweet_gic_ppi) {
		/*
		 * sweetgic=0: touch nothing in the GICR region.  This is the
		 * control arm -- if the boot reaches the desktop this way, the
		 * per-core SGI frame programming is not what keeps it alive.
		 */
		kprintf("SWEET GIC: cpu=%u GICR left untouched (sweetgic=0)\n", sweet_cpu);
		goto sweet_gicr_done;
	}

	/*
	 * 1. Which 64 KB page of the GICR region reports this core's affinity?
	 *    Reads only, and every read is guarded.  The stride is whatever the
	 *    hardware uses (0x10000 granularity covers 0x20000 and 0x40000).
	 */
	for (vm_offset_t off = 0; off + GICR_TYPER + 8 <= gicr_size; off += 0x10000) {
		uint32_t lo = 0, hi = 0;
		rd_probes++;
		if (sweet_probe_read32(gicr_base + off + GICR_TYPER, &lo) ||
		    sweet_probe_read32(gicr_base + off + GICR_TYPER + 4, &hi)) {
			rd_faults++;
			continue;
		}
		uint64_t typer = ((uint64_t)hi << 32) | (uint64_t)lo;
		uint32_t aff = (uint32_t)(typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) &
		    (uint32_t)(MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);
		if (aff == want_aff) {
			rd_off = off;
			rd_typer = typer;
			rd_found = 1;
			break;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
	}

	/*
	 * 2. Wake the redistributor: clear ProcessorSleep, then wait for
	 *    ChildrenAsleep to drop, bounded by gicr-wake-timeout.
	 */
	if (rd_found) {
		waker_before = _gic_read32(gicr_base + rd_off + GICR_WAKER);
		_gic_write32(gicr_base + rd_off + GICR_WAKER,
		    waker_before & ~(uint32_t)GICR_WAKER_PROCESSORSLEEP);
		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		for (;;) {
			waker_after = _gic_read32(gicr_base + rd_off + GICR_WAKER);
			if ((waker_after & GICR_WAKER_CHILDRENASLEEP) == 0) {
				wake_ok = 1;
				break;
			}
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				break;
			}
		}
	}

	/*
	 * 3. Where is the SGI/PPI page inside that frame?  Architected offset is
	 *    +0x10000; a vendor layout that pads the frame differently would put
	 *    it at +0x20000 or +0x30000, so all three are tried.
	 */
	if (rd_found) {
		static const vm_offset_t candidates[] = { 0x10000, 0x20000, 0x30000, 0 };
		for (unsigned int i = 0; candidates[i] != 0; i++) {
			uint32_t probe = 0;
			if (!sweet_probe_read32(
			    sweet_gicr_page(gicr_base + rd_off, candidates[i]) +
			    SWEET_GICR_SGI_ISENABLER, &probe)) {
				sgi_off = candidates[i];
				sgi_found = 1;
				break;
			}
		}
	}

	/*
	 * 4. Identity check: does the page actually hold SGI/PPI enable bits?
	 *    Writing all ones to GICR_ISENABLER0 and reading it back is the
	 *    cheapest way to tell a GIC SGI page from a hole or a foreign block.
	 *    A page that reads back zero implements none of INTIDs 0..31 and is
	 *    not used.
	 */
	if (sgi_found) {
		vm_offset_t isen = sweet_gicr_page(gicr_base + rd_off, sgi_off) +
		    SWEET_GICR_SGI_ISENABLER;
		if (!sweet_probe_write32(isen, 0xFFFFFFFFu) &&
		    !sweet_probe_read32(isen, &sgi_enables) && sgi_enables != 0) {
			sgi_writable = 1;
		}
	}

	kprintf("SWEET GIC: cpu=%u rd=0x%lx typer=0x%llx sgi=0x%lx wr=%u waker=0x%x->0x%x wake=%u en=0x%x faults=%u/%u\n",
	    sweet_cpu, (unsigned long)rd_off, (unsigned long long)rd_typer,
	    (unsigned long)sgi_off, sgi_writable, waker_before, waker_after,
	    wake_ok, sgi_enables, rd_faults, rd_probes);

	if (sgi_writable) {
		vm_offset_t sgi = sweet_gicr_page(gicr_base + rd_off, sgi_off);

		/* Start from a clean slate: nothing pending, nothing enabled. */
		_gic_write32(sgi + SWEET_GICR_SGI_ICENABLER, 0xFFFFFFFFu);

		/*
		 * Real hardware with two security states: Group 0 is TrustZone's
		 * and non-secure writes to it are ignored.  Everything we own is
		 * Group 1 NS and arrives as an IRQ (see sleh_irq).
		 * GICR_IGROUPR0 reads back as zero from the non-secure side even
		 * after this write; delivery still works.
		 */
		_gic_write32(sgi + SWEET_GICR_SGI_IGROUPR0, 0xFFFFFFFFu);
		for (unsigned int i = 0; i < 32; i += 4) {
			_gic_write32(sgi + SWEET_GICR_SGI_PRIORITY + i, 0xA0A0A0A0u);
		}
		_gic_write32(sgi + SWEET_GICR_SGI_ISENABLER,
		    (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
		kprintf("SWEET GIC: cpu=%u SGI page programmed at +0x%lx (ppi=%u sgi=%u)\n",
		    sweet_cpu, (unsigned long)sgi_off, sweet_vtimer_intid, SWEET_IPI_SGI);
	} else {
		/*
		 * Without the timeout PPI (and, for secondaries, the IPI SGI) the
		 * machine cannot make progress, and the panic text is the only
		 * diagnostic that survives on screen -- the boot log above it is
		 * pushed off by the panic dump.  Keep every number in one line.
		 */
		panic("SWEET GIC: cpu=%u no SGI page rd=0x%lx sgi=0x%lx typer=0x%llx en=0x%x waker=0x%x->0x%x wake=%u faults=%u/%u gicr=0x%lx size=0x%lx",
		    sweet_cpu, (unsigned long)rd_off, (unsigned long)sgi_off,
		    (unsigned long long)rd_typer, sgi_enables, waker_before,
		    waker_after, wake_ok, rd_faults, rd_probes,
		    (unsigned long)gicr_base, (unsigned long)gicr_size);
	}

sweet_gicr_done:
	;
#else
	vm_offset_t gicr_pe_base = find_gicr_pe_base();
	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));
#endif /* SWEET */

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

#if defined(SWEET)
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
#else
	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
#endif /* SWEET */
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

#if defined(SWEET)
	// ABL leaves only ARE_NS set; Linux writes ARE_NS | EnableGrp1A | EnableGrp1
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_ENABLEGRP1A | GICD_CTLR_ENABLEGRP1;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		;
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#else
	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#endif /* SWEET */
}
#else
void
pe_init_fiq()
{
}
#endif
