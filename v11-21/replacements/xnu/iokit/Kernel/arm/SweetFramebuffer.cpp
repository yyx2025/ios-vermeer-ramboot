/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h], 18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd presents the enabled layers' surfaces into the panel memory on the
 * vsync work loop (CPU copy, sweetfb_hz panel updates per second). Vsync
 * (notification type 5) is a 60 Hz timer that sends (count,
 * mach_absolute_time, 0) as 32-bit halves to clients that enabled it, through
 * the port they registered (IOConnectSetNotificationPort).
 *
 * v11.21 present path: the 1125x2436 canvas is expanded to the 1440x3200 panel
 * with a geometry-derived 25:32 run template instead of a per-pixel index
 * gather, rows are written only when their content or span changed, and the
 * time spent in present() is reported every 120 vsyncs. Boot arguments:
 *   sweetfb_hz=N        panel updates per second (1..60, default 10)
 *   sweetfb_rowskip=0   disable the row content cache
 *   sweetfb_stats=0     disable the periodic present() timing report
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: pausing it (boot-arg sweetfb_noconsole=1, on the
 * first swap) switches the console to serial, which also hides panics.
 */
#include "SweetSurface.h"

extern "C" int vermeer_console_muted;

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include <machine/atomic.h>
#include "vermeer_frame_policy.h"
#include "vermeer_frame_scale.h"
#include "K70FrameHandoff.h"
#include "K70PanelObserve.h"
extern "C" void vermeer_debug_frame_register(void (*)(void));
#if defined(K70_IOS16_IOKIT_ABI) && K70_IOS16_IOKIT_ABI
extern "C" IOReturn k70_gpu_start_worker(void);
#endif

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* presented iPhone X canvas */
#define VERMEER_LOGICAL_W 1125
#define VERMEER_LOGICAL_H 2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

/* v11.21 present fast path. The panel is 1440x3200 and the UIKit canvas
 * 1125x2436, so 1440/1125 == 32/25: every 25 source pixels expand to exactly
 * 32 panel pixels, and that run pattern repeats 45 times across the row. The
 * per-pixel divide+LUT gather is replaced by a geometry-derived template, the
 * row scratch is 8-byte aligned for paired stores, and a row is only written
 * to write-combining panel memory when its content actually changed. */
#define SWEET_FB_MAX_W                  1440    /* panel row, pixels */
#define SWEET_FB_MAX_PERIOD             64      /* largest supported run period */
#define SWEET_FB_STATS_EVERY            120     /* vsync ticks between reports */

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	int32_t       fOffX, fOffY; /* logical origin relative to the panel */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	SweetSurface *fCanvas;          /* logical default surface, always scaled */
	uint32_t      fScaleX[SWEET_FB_MAX_W]; /* dst x -> logical src x, built once */
	/* 8-byte aligned row scratch: the expansion stores pixel pairs without a
	 * second pass, and the row hash reads it as 64-bit words. */
	uint64_t      fScaleRow[(SWEET_FB_MAX_W / 2) + 8];
	uint32_t      fHRun[SWEET_FB_MAX_PERIOD]; /* panel px emitted per src px */
	uint32_t      fHPeriodSrc, fHPeriodDst, fHPeriods; /* 25 / 32 / 45 on K70 */
	uint64_t     *fRowHash;         /* per panel row: hash of last written span */
	uint32_t     *fRowCode;         /* per panel row: x0<<16 | x1 of that span */
	bool          fRowSkip;         /* honour the row content cache */
	bool          fRowCacheStale;   /* panel was written by something else */
	bool          fStats;           /* report present timing */
	uint32_t      fStatFrames, fStatRows, fStatSkipped;
	uint64_t      fStatNanos;
	IOLock       *fLock;            /* swaps and client list */
	K70PanelObserve fPanelObserve;
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	struct vframe_refs fPendingRefs; /* owning refs, transferred with the record */
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	/* v11.31: one preallocated hand-off copy. The vsync timer used to
	 * IOMallocData/IOFreeData this record on every flushed frame, which is a
	 * 1 KiB alloc/free pair at display rate -- exactly the kind of churn that
	 * fragments the kalloc zones of a kernel that is already running out of
	 * headroom. It is written and read only by the single vsync thread. */
	uint8_t      *fHandoff;
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	unsigned frameInputs(const uint8_t *rec, const struct vframe_refs *refs, K70FrameInput *frames, bool report);
	void present(const uint8_t *rec, const struct vframe_refs *refs);
	void invalidateRows();
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

static uint64_t _Atomic vf_submitted, vf_presented, vf_bytes, vf_missing, vf_rejected, vf_ticks;
static void vermeer_frame_status(void)
{
    IOLog("VERMEER FRAME: submits=%llu cpu-presents=%llu ticks=%llu\n",
        os_atomic_load(&vf_submitted,relaxed),os_atomic_load(&vf_presented,relaxed),os_atomic_load(&vf_ticks,relaxed));
    IOLog("VERMEER FRAME: default=logical1125x2436 panel=1440x3200 full-panel-v8\n");
    IOLog("VERMEER FRAME: copied-bytes=%llu missing=%llu rejected=%llu\n",
        os_atomic_load(&vf_bytes,relaxed),os_atomic_load(&vf_missing,relaxed),os_atomic_load(&vf_rejected,relaxed));
}
static void vermeer_frame_release(struct vframe_refs *refs)
{
    for (unsigned i=0;i<VFRAME_LAYERS;i++) {
        SweetSurface *surface=(SweetSurface *)refs->slot[i];
        if (surface) surface->release();
        refs->slot[i]=NULL;
    }
}

static uint32_t
sweet_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/*
 * Cheap, order-sensitive row digest. Four independent accumulators keep the
 * dependency chain off the critical path (one multiply per 32 bytes), and the
 * multiply-add mixer does not collide on reordered or permuted rows the way a
 * plain XOR fold would. A collision would only cost one stale panel row, and
 * the mixer makes it a ~2^-64 event per comparison.
 */
static inline uint64_t
sweet_row_hash(const uint32_t *pixels, uint32_t count)
{
	uint64_t a = 0x9e3779b97f4a7c15ULL, b = 0xc2b2ae3d27d4eb4fULL;
	uint64_t c = 0x165667b19e3779f9ULL, d = 0x27d4eb2f165667c5ULL;
	uint32_t i = 0;

	for (; i + 8 <= count; i += 8) {
		uint64_t w0, w1, w2, w3;
		memcpy(&w0, pixels + i + 0, 8);
		memcpy(&w1, pixels + i + 2, 8);
		memcpy(&w2, pixels + i + 4, 8);
		memcpy(&w3, pixels + i + 6, 8);
		a = (a ^ w0) * 0x100000001b3ULL;
		b = (b ^ w1) * 0x100000001b3ULL;
		c = (c ^ w2) * 0x100000001b3ULL;
		d = (d ^ w3) * 0x100000001b3ULL;
	}
	for (; i + 2 <= count; i += 2) {
		uint64_t w;
		memcpy(&w, pixels + i, 8);
		a = (a ^ w) * 0x100000001b3ULL;
	}
	if (i < count) {
		a = (a ^ (uint64_t)pixels[i]) * 0x100000001b3ULL;
	}
	return ((a ^ b) * 0x100000001b3ULL) ^ ((c ^ d) * 0x9e3779b1ULL);
}

/*
 * Full-panel row expansion for the 1125 -> 1440 geometry. Source pixels are
 * read once, in order, and each is stored as a pixel pair; the store pointer
 * advances by 1 or 2 so the duplicated columns land where floor(x*25/32)
 * puts them. The period table is built from the geometry at start(), never
 * from the per-frame clip rectangle, so the inner loop has no divides, no
 * 64-bit multiply and no destination index array.
 */
static void
sweet_expand_periodic(const uint8_t *row, uint32_t *dst, const uint32_t *runs,
    uint32_t periods, uint32_t periodSrc)
{
	const uint32_t *src = (const uint32_t *)row;
	uint32_t *out = dst;

	for (uint32_t p = 0; p < periods; p++) {
		for (uint32_t j = 0; j < periodSrc; j++) {
			uint32_t v = src[j];
			uint64_t pair = ((uint64_t)v << 32) | (uint64_t)v;
			/* One paired store covers both the single and the duplicated
			 * column; the spare half is overwritten by the next pixel. */
			memcpy(out, &pair, sizeof(pair));
			out += runs[j];
		}
		src += periodSrc;
	}
}

/* Integer panel/canvas ratio: replicate in place, no gather at all. */
static void
sweet_expand_replicate(const uint8_t *row, uint32_t *dst, uint32_t scale,
    uint32_t srcWidth)
{
	const uint32_t *src = (const uint32_t *)row;

	if (scale == 1) {
		memcpy(dst, src, (size_t)srcWidth * 4);
		return;
	}
	uint32_t *out = dst;
	for (uint32_t x = 0; x < srcWidth; x++) {
		uint32_t v = src[x];
		if (scale == 2) {
			uint64_t pair = ((uint64_t)v << 32) | (uint64_t)v;
			memcpy(out, &pair, sizeof(pair));
			out += 2;
		} else {
			out[0] = v;
			out[1] = v;
			out[2] = v;
			out += 3;
		}
	}
}

/* Clipped rows (partial width) keep the precomputed index walk. */
static void
sweet_expand_clipped(const uint8_t *row, uint32_t *dst, const struct vframe_scale *plan,
    const uint32_t *lut)
{
	for (uint32_t x = plan->x0; x < plan->x1; x++) {
		dst[x] = *(const uint32_t *)(row + (uint64_t)(lut[x] - plan->lx) * 4);
	}
}

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	/* Every early-exit below runs before the fast-path tables exist, and free()
	 * may be called on the way out: the pointers must never be indeterminate. */
	fRowHash = NULL;
	fRowCode = NULL;
	fRowSkip = false;
	fRowCacheStale = false;
	fStats = false;
	fConsolePaused = false;
	fStatFrames = fStatRows = fStatSkipped = 0;
	fStatNanos = 0;
	fHPeriods = 0;
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	/* Only the independently observed K70 framebuffer reservation. */
	if (fPhys != 0xb8000000ULL || fWidth != 1440 || fHeight != 3200 || fRowBytes != 5760) {
		IOLog("VERMEER FB: geometry guard refused\n");
		return false;
	}
	IOLog("VERMEER FB: geometry guard PASS\n");
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	/* Known UIKit layout, centered 1:1 inside the guarded K70 panel. */
	fOffX = (VERMEER_LOGICAL_W - (int32_t)fWidth) / 2;
	fOffY = (VERMEER_LOGICAL_H - (int32_t)fHeight) / 2;
	bzero(&fPendingRefs,sizeof(fPendingRefs));
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fCanvas = sweet_surface_create_buffer(VERMEER_LOGICAL_W, VERMEER_LOGICAL_H,
	    (VERMEER_LOGICAL_W * 4 + 63) & ~63u, SWEET_FB_FORMAT_BGRA);
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fCanvas == NULL || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fCanvas);
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
	fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);

	/*
	 * v11.21 present fast path.
	 * The panel and the UIKit canvas share the exact ratio 32:25, so the whole
	 * horizontal expansion is one 25-source-pixel period repeated 45 times.
	 * Build that period here, once, from the real geometry.
	 */
	uint32_t divisor = sweet_gcd(VERMEER_LOGICAL_W, fWidth);
	fHPeriodSrc = VERMEER_LOGICAL_W / divisor;
	fHPeriodDst = fWidth / divisor;
	fHPeriods = 0;
	fRowSkip = false;
	fRowCacheStale = false;
	fStats = false;
	fRowHash = NULL;
	fRowCode = NULL;
	fStatFrames = fStatRows = fStatSkipped = 0;
	fStatNanos = 0;
	if (fHPeriodSrc != 0 && fHPeriodSrc <= SWEET_FB_MAX_PERIOD && fWidth <= SWEET_FB_MAX_W &&
	    (VERMEER_LOGICAL_W % fHPeriodSrc) == 0 && (fWidth % fHPeriodSrc) == 0) {
		/* Destination columns whose floor(x*src/dst) is this source pixel.
		 * The image of the floor map is bounded by ceil(), not floor(): one
		 * source pixel covers two columns exactly where the boundary falls
		 * inside a column, which is why 1125 -> 1440 duplicates 7 of 25. */
		uint32_t emitted = 0;
		for (uint32_t j = 0; j < fHPeriodSrc; j++) {
			uint32_t next = (uint32_t)((((uint64_t)(j + 1) * fHPeriodDst) +
			    fHPeriodSrc - 1) / fHPeriodSrc);
			fHRun[j] = next - emitted;
			emitted = next;
		}
		if (emitted == fHPeriodDst &&
		    (uint64_t)(VERMEER_LOGICAL_W / fHPeriodSrc) * fHPeriodDst == fWidth) {
			fHPeriods = VERMEER_LOGICAL_W / fHPeriodSrc;
		}
	}
	for (uint32_t x = 0; x < fWidth && x < SWEET_FB_MAX_W; x++) {
		fScaleX[x] = (uint32_t)(((uint64_t)x * VERMEER_LOGICAL_W) / fWidth);
	}
	uint32_t skip = 1, stats = 1;
	PE_parse_boot_argn("sweetfb_rowskip", &skip, sizeof(skip));
	PE_parse_boot_argn("sweetfb_stats", &stats, sizeof(stats));
	/* The row cache is only valid while this driver is the sole writer of the
	 * panel memory; the boot console repaints it until it is muted. */
	int muted = 0;
	if (!PE_parse_boot_argn("sweetfb_noconsole", &muted, sizeof(muted)) || muted == 0) {
		skip = 0;
	}
	fStats = stats != 0;
	fRowHash = (uint64_t *)IOMallocData(sizeof(uint64_t) * fHeight);
	fRowCode = (uint32_t *)IOMallocData(sizeof(uint32_t) * fHeight);
	fHandoff = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
	if (fHandoff == NULL) {
		return false;
	}
	if (fRowHash != NULL && fRowCode != NULL && skip != 0) {
		invalidateRows();
		fRowSkip = true;
	} else {
		if (fRowHash != NULL) {
			IOFreeData(fRowHash, sizeof(uint64_t) * fHeight);
		}
		if (fRowCode != NULL) {
			IOFreeData(fRowCode, sizeof(uint32_t) * fHeight);
		}
		fRowHash = NULL;
		fRowCode = NULL;
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	/* Handoff remains inactive until the real Metal presenter is ready. */
	if (k70_frame_register(fSurface->memory,fPhys,fWidth,fHeight,fRowBytes)!=kIOReturnSuccess) return false;
	registerService();
#if defined(K70_IOS16_IOKIT_ABI) && K70_IOS16_IOKIT_ABI
	const IOReturn gpuStart=k70_gpu_start_worker();
	if(gpuStart!=kIOReturnSuccess && gpuStart!=kIOReturnBusy)
		IOLog("K70 GPU: could not schedule startup, status 0x%x\n",gpuStart);
#endif
	vermeer_debug_frame_register(vermeer_frame_status);
	IOLog("VERMEER FB: logical default surface %u 1125x2436; physical scanout separate v8\n", fCanvas->id);
	IOLog("VERMEER FB: retained pending frame ownership enabled\n");
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id);
	IOLog("VERMEER FB: logical canvas 1125x2436 scaled to full panel 1440x3200\n");
	IOLog("SWEET FB: present fast path %u->%u px period x%u, row cache %s, stats %s\n",
	    fHPeriodSrc, fHPeriodDst, fHPeriods, fRowSkip ? "on" : "off",
	    fStats ? "on" : "off");
	return true;
}

void
AppleCLCD::invalidateRows()
{
	if (fRowCode != NULL) {
		bzero(fRowCode, sizeof(uint32_t) * fHeight);
	}
	if (fRowHash != NULL) {
		bzero(fRowHash, sizeof(uint64_t) * fHeight);
	}
}

void
AppleCLCD::free()
{
	fPanelObserve.release();
	if (fHandoff != NULL) {
		IOFreeData(fHandoff, SWEET_FB_SWAP_REC_SIZE);
		fHandoff = NULL;
	}
	if (fRowHash != NULL) {
		IOFreeData(fRowHash, sizeof(uint64_t) * fHeight);
		fRowHash = NULL;
	}
	if (fRowCode != NULL) {
		IOFreeData(fRowCode, sizeof(uint32_t) * fHeight);
		fRowCode = NULL;
	}
	IOMobileFramebuffer::free();
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	fb->fPanelObserve.sample(fb->fPhys,fb->fWidth,fb->fHeight,fb->fRowBytes);
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	os_atomic_inc(&vf_ticks,relaxed);
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	struct vframe_refs refs = {};
	if (flush) {
		rec = fb->fHandoff;
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			vframe_take(&fb->fPendingRefs,&refs);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec,&refs);
		vermeer_frame_release(&refs);
	}
	sender->setTimeoutUS(SWEET_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
static uint64_t sweet_fb_swap_count;

/*
 * The GPU route snapshots before SwapEnd returns: retaining a SweetSurface
 * prevents deallocation, but does not prevent the producer reusing its pixels.
 * Only the CPU fallback defers scaling to the existing vsync timer.
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
    struct vframe_refs incoming={},displaced={};
    uint32_t mask=get32(rec,0x104);
    for (unsigned i=0;i<VFRAME_LAYERS;i++) {
        uint32_t id=get32(rec,0x54+4*i);
        if ((mask&(1u<<i)) && id) incoming.slot[i]=sweet_surface_copy(id);
    }
	IOLockLock(fLock);
    K70FrameInput frames[SWEET_FB_LAYERS] = {};
    const unsigned count=frameInputs(rec,&incoming,frames,true);
    const unsigned route=k70_frame_offer(frames,count);
    if(route==K70_FRAME_CPU) {
        vframe_replace(&fPendingRefs,&incoming,&displaced);
        memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
        fDirty = true;
    } else {
        vframe_take(&fPendingRefs,&displaced);
        fDirty = false;
    }
	IOLockUnlock(fLock);
    vermeer_frame_release(&incoming);
    vermeer_frame_release(&displaced);
    os_atomic_inc(&vf_submitted,relaxed);
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

unsigned
AppleCLCD::frameInputs(const uint8_t *rec, const struct vframe_refs *refs, K70FrameInput *frames, bool report)
{
	uint32_t mask = get32(rec, 0x104);
	unsigned frameCount = 0;
	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = (SweetSurface *)refs->slot[layer];
		if (report && sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
            if (report && !s) os_atomic_inc(&vf_missing,relaxed);
			continue;
		}
        uintptr_t src=s->kernelAddress();
        uint32_t bpe=s->bytesPerElement ? s->bytesPerElement : 4;
        if (w<=0 || h<=0) { w=(int32_t)s->width;h=(int32_t)s->height; }
        struct vframe_scale plan;
        if (!src || bpe!=4 || s->elementWidth!=1 || s->elementHeight!=1 || !s->memory ||
            !vframe_scale_plan(&plan,sx,sy,dx,dy,w,h,s->width,s->height,s->bytesPerRow,
                s->offset,s->memory->getLength(),VERMEER_LOGICAL_W,VERMEER_LOGICAL_H,
                fWidth,fHeight,fRowBytes,(uint64_t)fRowBytes*fHeight)) {
            if(report)os_atomic_inc(&vf_rejected,relaxed);continue;
        }
        frames[frameCount++]={(const uint8_t *)src,s->memory->getLength(),s->bytesPerRow,plan};
	}
    return frameCount;
}

void
AppleCLCD::present(const uint8_t *rec, const struct vframe_refs *refs)
{
    /* ARM and GPU claims use this same lock, so a stale CPU route cannot
     * authorize a clear/copy after the GPU has taken ownership. */
    if(!k70_frame_cpu_begin()){
        /* Another owner wrote the panel: the row cache no longer describes it. */
        fRowCacheStale=true;return;
    }
    if(fRowCacheStale){
        invalidateRows();fRowCacheStale=false;
    }
    os_atomic_inc(&vf_presented,relaxed);
    uint64_t started=0;
    if(fStats)absolutetime_to_nanoseconds(mach_absolute_time(),&started);
    if (!fConsolePaused) {
        int quiet = 0;
        fConsolePaused = true;
        if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
            IOLog("VERMEER FB: first present; muting panel text only\n");
            vermeer_console_muted = 1;
            bzero((void *)fPixels, (size_t)fRowBytes * fHeight);
            /* The panel no longer holds what the row cache describes. */
            invalidateRows();
        }
    }
    uint32_t *scratch=(uint32_t *)fScaleRow;
    K70FrameInput frames[SWEET_FB_LAYERS] = {};
    const unsigned frameCount=frameInputs(rec,refs,frames,false);
    for(unsigned i=0;i<frameCount;++i) {
        const K70FrameInput &frame=frames[i];const vframe_scale &plan=frame.scale;
        const uint32_t width=plan.x1-plan.x0;
        if(width==0||width>SWEET_FB_MAX_W)continue;
        /* Nothing below depends on the clip rectangle any more: the source
         * index table was built once in start() and the span only selects
         * which part of the row is emitted. */
        const uint32_t code=(plan.x0<<16)|(plan.x1&0xffffu);
        /* Row builders that regenerate the whole row from column 0 need an
         * unclipped span and a source row that starts at column 0. */
        const bool fromColumn0=plan.lx==0&&frame.stride!=0&&(plan.src%frame.stride)==0;
        const bool periodic=fromColumn0&&plan.x0==0&&plan.x1==fWidth&&
            plan.lw==VERMEER_LOGICAL_W&&fHPeriods!=0;
        const uint32_t replicate=(!fromColumn0||plan.pw>SWEET_FB_MAX_W)?0:
            ((plan.pw==plan.lw)?1:
            ((plan.pw==plan.lw*2&&plan.ph==plan.lh*2)?2:
            ((plan.pw==plan.lw*3&&plan.ph==plan.lh*3)?3:0)));
        uint32_t previous=UINT32_MAX;
        uint64_t rowHash=0;
        for (uint32_t y=plan.y0;y<plan.y1;y++) {
            uint32_t source_y=(uint32_t)(((uint64_t)y*plan.lh)/plan.ph)-plan.ly;
            if(source_y!=previous) {
                const uint8_t *row=frame.base+plan.src+(uint64_t)source_y*frame.stride;
                if(replicate){
                    /* Integer panel/canvas ratio: replicate, 1:1 is a memcpy. */
                    sweet_expand_replicate(row,scratch,replicate,plan.lw);
                }else if(periodic){
                    sweet_expand_periodic(row,scratch,fHRun,fHPeriods,fHPeriodSrc);
                }else{
                    sweet_expand_clipped(row,scratch,&plan,fScaleX);
                }
                rowHash=sweet_row_hash(scratch+plan.x0,width);
                previous=source_y;
            }
            /* A row whose content and span are unchanged is already on the
             * panel: skip the write-combining burst entirely. */
            if(fRowSkip&&fRowCode[y]==code&&fRowHash[y]==rowHash){
                fStatSkipped++;continue;
            }
            memcpy((void *)(fPixels+(uint64_t)y*fRowBytes+(uint64_t)plan.x0*4),
                scratch+plan.x0,(size_t)width*4);
            if(fRowSkip){
                fRowCode[y]=code;fRowHash[y]=rowHash;
            }
            fStatRows++;
        }
        os_atomic_add(&vf_bytes,(uint64_t)width*(plan.y1-plan.y0)*4,relaxed);
	}
    if(fStats){
        uint64_t finished=0;
        absolutetime_to_nanoseconds(mach_absolute_time(),&finished);
        fStatNanos+=finished-started;
        if(++fStatFrames>=SWEET_FB_STATS_EVERY){
            const uint64_t rows=(uint64_t)fStatRows+fStatSkipped;
            IOLog("SWEET FB: present %u frames rows=%u skipped=%u rows/frame=%llu avg=%lluus\n",
                fStatFrames,fStatRows,fStatSkipped,rows/fStatFrames,
                fStatNanos/((uint64_t)fStatFrames*1000));
            fStatFrames=fStatRows=fStatSkipped=0;
            fStatNanos=0;
        }
    }
    k70_frame_cpu_end();
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fCanvas, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = VERMEER_LOGICAL_W;
		out[1] = VERMEER_LOGICAL_H;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
