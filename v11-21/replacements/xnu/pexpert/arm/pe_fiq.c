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
static uint32_t sweet_gic_mode = 1;      /* 0=off 1=full 2=wake-only */
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
 * vermeer GICR geometry.
 *
 * The ADT carries only "/arm-io/gic reg", so the redistributor frame stride is
 * not visible at runtime.  The vendor DTB has it:
 *
 *   /soc/interrupt-controller@17100000
 *       compatible = "arm,gic-v3"
 *       reg = <0x17100000 0x10000    (GICD)
 *              0x17180000 0x200000>   (GICR)
 *       #redistributor-regions = <1>
 *       redistributor-stride = <0x0 0x40000>
 *
 * 8 x 0x40000 == 0x200000 fills the GICR window exactly, so on kalama every
 * redistributor frame is 256 KB.  XNU's GICR_PE_SIZE is 0x20000, which is the
 * SM8450/yupik layout (that DTB declares 0x20000 over a 0x100000 window); using
 * it here makes every secondary program somebody else's frame.
 *
 * Both numbers stay overridable from the boot arguments so the layout can be
 * re-tested without relinking the kernel:
 *
 *   sweetgicstride=0x40000   redistributor frame stride   (default 0x40000)
 *   sweetgicsgi=0x10000      SGI/PPI page inside the frame (default 0x10000)
 *   sweetgic=1               full programming                (default)
 *   sweetgic=0               leave the GICR region untouched
 *   sweetgic=2               wake the redistributor only, no SGI/PPI writes
 */
#define SWEET_GICR_STRIDE_DEFAULT 0x40000
#define SWEET_GICR_SGI_DEFAULT    0x10000

/* Frame-relative offsets (GICv3). */
#define SWEET_GICR_SGI_IGROUPR0  0x0080
#define SWEET_GICR_SGI_ISENABLER 0x0100
#define SWEET_GICR_SGI_ICENABLER 0x0180
#define SWEET_GICR_SGI_PRIORITY  0x0400

static vm_offset_t sweet_gicr_stride = SWEET_GICR_STRIDE_DEFAULT;
static vm_offset_t sweet_gicr_sgi_off = SWEET_GICR_SGI_DEFAULT;

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
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

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
		uint32_t sweet_arg = 1;
		if (PE_parse_boot_argn("sweetgic", &sweet_arg, sizeof(sweet_arg))) {
			sweet_gic_mode = sweet_arg > 2 ? 1 : sweet_arg;
		}
		uint32_t sweet_stride_arg = SWEET_GICR_STRIDE_DEFAULT;
		if (PE_parse_boot_argn("sweetgicstride", &sweet_stride_arg, sizeof(sweet_stride_arg))) {
			sweet_gicr_stride = (vm_offset_t)sweet_stride_arg;
		}
		uint32_t sweet_sgi_arg = SWEET_GICR_SGI_DEFAULT;
		if (PE_parse_boot_argn("sweetgicsgi", &sweet_sgi_arg, sizeof(sweet_sgi_arg))) {
			sweet_gicr_sgi_off = (vm_offset_t)sweet_sgi_arg;
		}
		kprintf("SWEET GIC: soc=0x%lx gicd_pa=0x%lx gicr_pa=0x%lx gicr_size=0x%lx stride=0x%lx sgi=0x%lx mode=%u\n",
		    (unsigned long)soc_base_phys,
		    (unsigned long)(soc_base_phys + gicd_base_prop),
		    (unsigned long)(soc_base_phys + gicr_base_prop),
		    (unsigned long)gicr_size_prop,
		    (unsigned long)sweet_gicr_stride, (unsigned long)sweet_gicr_sgi_off,
		    sweet_gic_mode);
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
	 * v11.28: pick the frame from the DTB stride instead of walking TYPERs.
	 *
	 * History that forced this shape:
	 *  - v11.23 computed frame = cpu * GICR_PE_SIZE (0x20000).  Wrong stride,
	 *    and the pre-walk read of an unimplemented frame produced a bus error
	 *    that XNU reports as "Undefined kernel instruction".
	 *  - v11.24 woke the redistributor unconditionally, which was correct but
	 *    not sufficient: the first store still aborted.
	 *  - v11.25 tried to discover the layout by probing with a fault-tolerant
	 *    accessor, which required rebuilding osfmk/arm64/sleh.c.  That rebuild
	 *    flipped sleh.c's "#if USE_APPLEARMSMP" (the macro only exists when a
	 *    board config is on the command line) and replaced the shipped
	 *    icc_iar1_el1 interrupt path with a call through
	 *    cpu_data.interrupt_handler, which is never initialised in an AppleSMP
	 *    kernel.  The very first interrupt branched to address 0.
	 *
	 * So: no probing, no sleh.c.  The stride is a platform fact, the SGI page
	 * offset is architectural (+0x10000), and both are boot-argument
	 * overridable so a wrong value costs one reboot instead of a rebuild.
	 */
	unsigned int sweet_cpu = ml_get_cpu_number_local();
	vm_offset_t rd_off = (vm_offset_t)sweet_cpu * sweet_gicr_stride;
	uint64_t rd_typer = 0;
	uint32_t want_aff = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") &
	    (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK));
	uint32_t rd_aff = 0xFFFFFFFFu;
	uint32_t waker_before = 0, waker_after = 0, sgi_enables = 0;
	unsigned int wake_ok = 0;
	unsigned int aff_ok = 0;
	unsigned int rd_in_range = 0;

	if (sweet_gic_mode == 0) {
		/*
		 * Control arm: touch nothing in the GICR region.  ICC_SRE/PMR/
		 * IGRPEN1 and GICD_CTLR below still run, so the boot continues
		 * with only the virtual-timer PPI and the IPI SGI disabled.
		 */
		kprintf("SWEET GIC: cpu=%u GICR left untouched (sweetgic=0)\n", sweet_cpu);
		goto sweet_gicr_done;
	}

	/*
	 * 1. Bounds.  A stride that walks past the mapped window would turn a
	 *    configuration mistake into a kernel data abort on a bogus address.
	 */
	if (rd_off + sweet_gicr_sgi_off + SWEET_GICR_SGI_ISENABLER + 4 <= gicr_size) {
		rd_in_range = 1;
	}

	/*
	 * 2. Identity.  GICR_TYPER lives in the RD control page next to WAKER,
	 *    so this read cannot be the access that aborts.  It is also the only
	 *    defence against a wrong stride: a frame that reports a different
	 *    affinity is not ours.
	 */
	if (rd_in_range) {
		rd_typer = _gic_read64(gicr_base + rd_off + GICR_TYPER);
		rd_aff = (uint32_t)(rd_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) &
		    (uint32_t)(MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);
		aff_ok = (rd_aff == want_aff);
	}

	/*
	 * 3. Wake.  A cold-boot redistributor answers WAKER = 0x6
	 *    (ProcessorSleep | ChildrenAsleep); clear ProcessorSleep and wait for
	 *    ChildrenAsleep to drop, bounded by gicr-wake-timeout.
	 */
	if (aff_ok) {
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

	kprintf("SWEET GIC: cpu=%u stride=0x%lx rd=0x%lx typer=0x%llx aff=0x%x want=0x%x inrange=%u waker=0x%x->0x%x wake=%u mode=%u gicr_size=0x%lx\n",
	    sweet_cpu, (unsigned long)sweet_gicr_stride, (unsigned long)rd_off,
	    (unsigned long long)rd_typer, rd_aff, want_aff, rd_in_range,
	    waker_before, waker_after, wake_ok, sweet_gic_mode,
	    (unsigned long)gicr_size);

	if (sweet_gic_mode == 2) {
		/* Wake-only arm: proves whether the RD control page answers writes
		 * before any SGI-page store is attempted. */
		kprintf("SWEET GIC: cpu=%u wake-only, SGI page untouched (sweetgic=2)\n", sweet_cpu);
		goto sweet_gicr_done;
	}

	if (wake_ok) {
		vm_offset_t sgi = sweet_gicr_page(gicr_base + rd_off, sweet_gicr_sgi_off);

		/*
		 * Start from a clean slate: nothing pending, nothing enabled.  The
		 * read-back tells us whether the SGI page accepts stores at all --
		 * a page that reads back zero implements none of INTIDs 0..31.
		 */
		_gic_write32(sgi + SWEET_GICR_SGI_ICENABLER, 0xFFFFFFFFu);
		sgi_enables = _gic_read32(sgi + SWEET_GICR_SGI_ICENABLER);

		/*
		 * Real hardware with two security states: Group 0 belongs to
		 * TrustZone and non-secure writes to it are ignored.  Everything we
		 * own is Group 1 NS and arrives as an IRQ (see sleh_irq).
		 * GICR_IGROUPR0 reads back as zero from the non-secure side even
		 * after this write; delivery still works.
		 */
		_gic_write32(sgi + SWEET_GICR_SGI_IGROUPR0, 0xFFFFFFFFu);
		for (unsigned int i = 0; i < 32; i += 4) {
			_gic_write32(sgi + SWEET_GICR_SGI_PRIORITY + i, 0xA0A0A0A0u);
		}
		_gic_write32(sgi + SWEET_GICR_SGI_ISENABLER,
		    (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
		kprintf("SWEET GIC: cpu=%u SGI page programmed at +0x%lx (ppi=%u sgi=%u) icen=0x%x\n",
		    sweet_cpu, (unsigned long)sweet_gicr_sgi_off, sweet_vtimer_intid,
		    SWEET_IPI_SGI, sgi_enables);
	} else {
		/*
		 * No usable redistributor: the timeout PPI (and, for secondaries,
		 * the IPI SGI) stays off.  Log and continue -- the panic dump would
		 * push the boot log off the screen, and the boot log is the only
		 * diagnostic we get.
		 */
		kprintf("SWEET GIC: cpu=%u SGI frame skipped (inrange=%u aff=0x%x want=0x%x wake=%u)\n",
		    sweet_cpu, rd_in_range, rd_aff, want_aff, wake_ok);
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
