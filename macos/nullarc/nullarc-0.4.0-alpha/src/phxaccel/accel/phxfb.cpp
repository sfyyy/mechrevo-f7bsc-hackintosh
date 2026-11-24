// PhxFB: the 780M's display as an IOFramebuffer that PhxAccel (NMXAccel) composites for WindowServer.
// Like NullMoth's NMIntelFB (phase 1), it takes over the mode the firmware (GOP) already lit, at boot in place of the
// firmware framebuffer (IONDRV), and writes nothing to the display engine: the scan-out surface is the boot console's
// (IOPlatformExpert::getConsoleInfo), the accelerator's display pipe copies each finished frame into a write-combined CPU
// mapping of it, and a timer at the panel's refresh (measured on the display engine, measureRefresh) stands in for the VBL
// interrupt WindowServer paces an accelerated display on.
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include "nvrm_vram_abi.h"
#include "nvrm_gpuva_abi.h"

void phxAgdcInstall();   // phxagdc.cpp: the AppleGraphicsDeviceControl that reports this panel (debug.phxagdc)
void phxAgdcRemove();
void phxHoldInit(IOService *fb, unsigned bootHoldSec);   // phxagdc.cpp: debug.phxfb_hold keeps this framebuffer (and so the GPU) busy

#define PFBLOG(fmt, ...) IOLog("PhxFB: " fmt "\n", ##__VA_ARGS__)

static int gAlias = 1;
static bool gAliasSysctl;
SYSCTL_INT(_debug, OID_AUTO, phxfb_alias, CTLFLAG_RW | CTLFLAG_LOCKED, &gAlias, 0,
           "1 = frames are copied into the scan-out through its system-RAM address (the carve-out), 0 = through the GPU's BAR");

// The VBL tick. XNU's timer coalescing moves a kernel deadline by up to an eighth of its delay, at most 1 ms, so a
// 120 Hz tick could land anywhere in the first millisecond after the vblank it stands for. The system-critical urgency
// that is never moved is not exported to kexts (thread_call_enter_delayed_with_leeway), so the tick is armed 1.5 ms
// early and re-armed for its deadline from there, each stage with an eighth of the previous slop, until less than
// 50 us remain (debug.phxfb_vbl_staged, 0 = one plain deadline). The lateness of each tick is summed for the
// measurements, and nvrm-accel.cpp times its presents against the last tick (gPhxVblLastAbs, gPhxFrameAbs).
static int gVblStaged = 1;
static SInt64 gVblN, gVblLateUsSum, gVblLateUsMax;
uint64_t gPhxVblLastAbs, gPhxFrameAbs;
volatile uint64_t gPhxVblSeq;   // ticks so far, counted before the handlers run: the display pipe completes on the next one
SYSCTL_INT(_debug, OID_AUTO, phxfb_vbl_staged, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblStaged, 0,
           "1 = the VBL tick is armed early and re-armed until its deadline (little coalescing slop), 0 = one plain deadline");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gVblN, "VBL ticks fired");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_sum, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsSum, "microseconds the VBL ticks fired past their deadlines, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblLateUsMax, "the latest VBL tick, microseconds past its deadline (writable: 0 restarts it)");
// Exact VBL timestamps (f7bsc patch). IOFramebuffer's VBL handler stamps vblTime/vblDelta with mach_absolute_time() at
// the moment the tick calls it, so the tick's wake-up latency (0-200 us) went straight into vblDelta (16.51-16.82 ms
// seen through SLSDisplayGetCurrentVBLDelta). CoreVideo takes its refresh period from that delta, and its video/host
// time model then wandered by seconds: CVDisplayLink output hostTimes +-10 s off, which Chrome uses as its vsync
// timebase (2026-11-19). With spin > 0 the last stage is armed spin us before the deadline and busy-waits for it, so the
// handlers run within a microsecond or two of the vblank the tick stands for (~1.2% of one core at 60 Hz, spin 200).
// 0 = the old staging (fire as soon as less than 50 us remain). jitter_us_max: the worst |fire - deadline| so far.
static int gTiming = 1;
SYSCTL_INT(_debug, OID_AUTO, phxfb_timing, CTLFLAG_RW | CTLFLAG_LOCKED, &gTiming, 0,
           "1 = the mode reports detailed timing (fixed VBL interval for IOFramebuffer/CoreVideo), 0 = none; read at display setup");
static int gVblSpinUs = 0;   // off: exact stamps made CVDisplayLink skip MORE frames (A/B 2026-11-19, 890 vs 687-777 callbacks/15 s)
static SInt64 gVblJitterUsMax;
SYSCTL_INT(_debug, OID_AUTO, phxfb_vbl_spin_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblSpinUs, 0,
           "microseconds the VBL tick busy-waits for its deadline so handlers see exact vblank times (0 = off, max 1000)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_jitter_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblJitterUsMax,
            "the worst distance of a VBL tick's handler call from its deadline, microseconds, early or late (writable: 0 restarts it)");
// Double-buffered scan-out (f7bsc patch). The display pipe used to copy every frame straight into the one surface DCN is
// scanning, at a moment set by the software VBL tick, which has no phase relation to the panel's real vblank: the copy
// raced the scan line and fast motion tore. With this on, the copy goes into a second surface in the carve-out and
// HUBP0's primary address is then switched to it; DCN latches the new address at VUPDATE (in vblank), so a frame is
// never shown half old, half new. debug.phxfb_dbuf: 0 = off (one surface, as before), 1 = on, N > 1 = on for N seconds
// and then back to the original surface by itself (a trial that undoes itself if the panel shows garbage).
// Everything the code assumes is read back from the display engine first (debug.phxfb_dbuf_state says which check
// refused); any surprise turns it off and leaves the panel on the firmware's surface.
static int gDbuf = 0;
static int gDbufOffMb = 512;          // carve-out offset of the second surface, MiB (above the 256 MiB BAR window PhxBringup uses)
static int gDbufState = 0;            // 1 active, 0 not tried, < 0 the check that refused (see dbufSetup)
static int gDbufWaitMaxUs = 0;
static SInt64 gDbufFlips, gDbufWaits, gDbufWaitTimeouts, gDbufHomes, gDbufAdopted;
static uint64_t gDbufTrialEnd = 0;    // mach_absolute_time at which a trial (gDbuf > 1) ends
static volatile SInt32 gDbufGen = 0;  // bumped by every write of debug.phxfb_dbuf: a refused setup is tried again
static int phxfb_dbuf_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gDbuf;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0) return EINVAL;
    if (v > 1) {
        uint64_t d = 0; clock_interval_to_deadline((uint32_t)(v > 3600 ? 3600 : v), kSecondScale, &d);
        gDbufTrialEnd = d;
    } else gDbufTrialEnd = 0;
    gDbuf = v;
    OSIncrementAtomic(&gDbufGen);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxfb_dbuf, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxfb_dbuf_sysctl, "I",
            "double-buffered scan-out: 0 off, 1 on, N>1 on for N seconds then back to one surface");
SYSCTL_INT(_debug, OID_AUTO, phxfb_dbuf_off_mb, CTLFLAG_RW | CTLFLAG_LOCKED, &gDbufOffMb, 0,
           "carve-out offset of the second scan-out surface in MiB (read when double buffering starts)");
SYSCTL_INT(_debug, OID_AUTO, phxfb_dbuf_state, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufState, 0,
           "1 active, 0 not started, <0 refused: -1 no MMIO, -2 no carve-out info/alias, -3 HUBP address, -4 pitch, -5 FB window, "
           "-6 system aperture, -7 map, -8 update lock, -9 carve-out too small, -10 HUBP reprogrammed by someone else, -11 tiled");
SYSCTL_INT(_debug, OID_AUTO, phxfb_dbuf_wait_max_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gDbufWaitMaxUs, 0,
           "longest wait for the previous flip to latch, us");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_dbuf_flips, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufFlips, "frames flipped to the panel");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_dbuf_waits, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufWaits, "frames that waited for the previous flip to latch");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_dbuf_wait_timeouts, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufWaitTimeouts, "waits that gave up (frame dropped)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_dbuf_homes, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufHomes, "returns to the firmware's surface");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_dbuf_adopted, CTLFLAG_RD | CTLFLAG_LOCKED, &gDbufAdopted, "times HUBP was found on the other surface and followed");

static struct sysctl_oid *const kFbOids[] = { &sysctl__debug_phxfb_alias, &sysctl__debug_phxfb_vbl_staged, &sysctl__debug_phxfb_vbl_n,
                                              &sysctl__debug_phxfb_vbl_late_us_sum, &sysctl__debug_phxfb_vbl_late_us_max,
                                              &sysctl__debug_phxfb_vbl_spin_us, &sysctl__debug_phxfb_vbl_jitter_us_max, &sysctl__debug_phxfb_timing,
                                              &sysctl__debug_phxfb_dbuf, &sysctl__debug_phxfb_dbuf_off_mb, &sysctl__debug_phxfb_dbuf_state,
                                              &sysctl__debug_phxfb_dbuf_wait_max_us, &sysctl__debug_phxfb_dbuf_flips,
                                              &sysctl__debug_phxfb_dbuf_waits, &sysctl__debug_phxfb_dbuf_wait_timeouts,
                                              &sysctl__debug_phxfb_dbuf_homes, &sysctl__debug_phxfb_dbuf_adopted };

// a frame copy with non-temporal stores: the carve-out views come out write-back, and plain stores left frames in the
// CPU cache while DCN read stale DRAM (the stripes of 2026-10-20). Both sides 8-byte aligned, bytes a multiple of 8.
static void phxNtCopy(void *dst, const void *src, uint64_t bytes)
{
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    for (uint64_t i = 0, n = bytes / 8; i < n; i++) __asm__ __volatile__("movnti %1, %0" : "=m"(d[i]) : "r"((uint64_t)s[i]));
    __asm__ __volatile__("sfence" ::: "memory");
}

// The panel's backlight is the display block's PWM (DCN 3.1.4 PWRSEQ0, segment 2 base 0x34c0): ACPI's _BCM only asks the
// GPU driver through ATIF (AFN7), and the firmware leaves the PWM on at full duty (period 4000, 12 bits). It is set as
// Linux's panel_cntl does without DMUB: lock the group, write the active count, unlock, wait for the pending update.
// tools/phxctl/phxbl does the same from user space. -phxbloff leaves the panel without a brightness control.
enum : uint32_t { kBlCntl = 0x34c0 + 0x2f19, kBlPeriod = 0x34c0 + 0x2f1b, kBlLock = 0x34c0 + 0x2f1c };
// The firmware runs both PWM generators, PWRSEQ0 and PWRSEQ1 (+0x6c), at full duty, and PWRSEQ0 alone left the panel's
// brightness as it was (2026-10-25: eDP DPCD 0x721 says the panel follows its PWM pin): both get the level.
static constexpr uint32_t kBlSeqs[] = { 0, 0x2f85 - 0x2f19 };
static constexpr uint32_t kBlMax = 1024;   // the brightness parameter's range
class PhxAMDFB;
static IOService *phxBacklightCreate(PhxAMDFB *fb);

// The class name carries "AMD": CoreDisplay tells the GPU vendor from the framebuffer's class name (Intel, AMD, NVDA,
// AppleParavirt), and for any other name it turns IOPresentment off, which left the panel on the software compositor
// with the Metal compositor up for the accelerator (GetGPUVendorForFramebufferService -> 1, 2026-10-20).
class PhxAMDFB : public IOFramebuffer {
    OSDeclareDefaultStructors(PhxAMDFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface through the BAR
    // The same surface through its system-RAM address: the 780M's VRAM is a carve-out of system memory, and amdgpu maps
    // it there on APUs rather than through the BAR. Through the BAR a 2560x1600 frame took ~110 ms to copy (~7 fps, the
    // whole desktop lagged, 2026-10-20). This view comes out write-back although it asks for write-combined (frames
    // copied with plain stores stayed in the CPU cache: stripes), so the display pipe writes it with non-temporal stores.
    IOMemoryMap *fAliasMap = nullptr;
    IOMemoryMap *fMmio = nullptr;      // BAR5, for the backlight PWM
    IOService *fBacklight = nullptr;   // the brightness parameter handler, named "backlight"
    unsigned fAliasTries = 0;
    volatile UInt32 fAliasBusy = 0;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl[4], fConnect;   // IOFramebuffer itself and, when it asks, IOAccelDisplayPipe each register a VBL handler
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    bool fBeside = false;   // another framebuffer (the firmware's IONDRV one) already drives this GPU: not the console
    IOPhysicalAddress fBase = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0;
    static constexpr uint64_t kFrameNs = 16666667ull;   // 60 Hz, when the refresh cannot be measured
    uint64_t fFrameNs = kFrameNs;
    uint32_t fRefreshMilliHz = 60000, fHTotal = 0, fVTotal = 0;
    bool fTimingLogged = false;
    static constexpr unsigned kBootHoldSec = 120;   // the boot daemon releases it sooner (debug.phxfb_hold=0)
    // The accelerator is NullMoth's, written against an NVIDIA RM: it backs each IOAccel surface with "VRAM" from the
    // framebuffer (nvAllocVram) and a GPU address from the RM (nvGpuVaAlloc/Map). On the 780M nothing on the GPU reads
    // those surfaces -- the display pipe copies finished frames by CPU, and RADV keeps its own GPU addresses in
    // PhxBringup -- so VRAM here is physically contiguous system memory (the 780M has no other kind) and a GPU address
    // is a number from a private range that never enters a page table. Without them every surface_flush WindowServer
    // makes failed ("failed to alloc GART space"), 30 times a second, and no frame reached the panel (2026-10-20).
    static constexpr uint64_t kVaBase = 0x100000000000ull, kVaSize = 1ull << 40, kVramCap = 1024ull << 20;
    IORangeAllocator *fVa = nullptr;
    volatile SInt64 fVramBytes = 0;
    volatile SInt32 fHandles = 0;
    unsigned fRmLogged = 0;
    // VRAM comes from one physically contiguous pool taken at start, while memory is still unfragmented: allocated one
    // by one, a 16 MB surface found no contiguous run hours into a session, and when WindowServer re-allocated its frame
    // on display wake the panel kept the frame from before the display slept (2026-10-21). An allocation is a
    // sub-descriptor of the pool (its handle); past the pool each one is contiguous memory of its own, as before.
    static constexpr uint64_t kPoolTry[3] = { 512ull << 20, 256ull << 20, 128ull << 20 };
    IOBufferMemoryDescriptor *fPool = nullptr;
    IORangeAllocator *fPoolRange = nullptr;
    uint8_t *fPoolKva = nullptr;
    uint64_t fPoolPhys = 0, fPoolBytes = 0;

    void poolInit() {
        for (uint64_t bytes : kPoolTry) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
            if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
            if (!b) continue;
            fPoolRange = IORangeAllocator::withRange(bytes, PAGE_SIZE, 0, IORangeAllocator::kLocking);
            if (!fPoolRange) { b->complete(); b->release(); return; }
            fPool = b; fPoolKva = (uint8_t *)b->getBytesNoCopy(); fPoolBytes = bytes;
            fPoolPhys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            PFBLOG("VRAM pool: %llu MB at phys 0x%llx", bytes >> 20, fPoolPhys);
            return;
        }
        PFBLOG("VRAM pool: no contiguous run of %llu MB or less, every allocation on its own", kPoolTry[2] >> 20);
    }
    void poolFree() {
        OSSafeReleaseNULL(fPoolRange);
        if (fPool) { fPool->complete(); OSSafeReleaseNULL(fPool); }
        fPoolKva = nullptr; fPoolPhys = fPoolBytes = 0;
    }
    bool poolAlloc(NVRMVramRequest *r, uint64_t bytes) {
        IORangeScalar off = 0;
        if (!fPoolRange || !fPoolRange->allocate(bytes, &off, PAGE_SIZE)) return false;
        IOSubMemoryDescriptor *s = IOSubMemoryDescriptor::withSubRange(fPool, off, bytes, kIODirectionInOut);
        if (!s) { fPoolRange->deallocate(off, bytes); return false; }
        bzero(fPoolKva + off, bytes);
        r->handle = s; r->kva = fPoolKva + off; r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = fPoolPhys + off;
        return true;
    }

    IOReturn vramAlloc(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION || !r->size) return kIOReturnBadArgument;
        const uint64_t bytes = round_page_64(r->size);
        if ((uint64_t)OSAddAtomic64((SInt64)bytes, &fVramBytes) + bytes > kVramCap) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes refused, %lld already out (cap %llu)", bytes, fVramBytes, kVramCap);
            return kIOReturnNoMemory;
        }
        if (poolAlloc(r, bytes)) {
            r->mappedTotal = (uint64_t)fVramBytes;
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx from the pool (%lld out)", bytes, r->phys, fVramBytes);
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
        if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
        if (!b) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: no contiguous %llu bytes", bytes);
            return kIOReturnNoMemory;
        }
        bzero(b->getBytesNoCopy(), bytes);
        r->handle = b; r->kva = b->getBytesNoCopy(); r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        r->mappedTotal = (uint64_t)fVramBytes;
        if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx (%lld out)", bytes, r->phys, fVramBytes);
        return kIOReturnSuccess;
    }
    IOReturn vramFree(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (IOSubMemoryDescriptor *s = OSDynamicCast(IOSubMemoryDescriptor, (OSObject *)r->handle)) {
            const uint64_t off = (uint64_t)((uint8_t *)r->kva - fPoolKva), len = s->getLength();
            if (!fPoolRange || (uint8_t *)r->kva < fPoolKva || off + len > fPoolBytes) return kIOReturnBadArgument;
            OSAddAtomic64(-(SInt64)len, &fVramBytes);
            fPoolRange->deallocate(off, len);
            s->release();
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)r->handle);
        if (!b) return kIOReturnBadArgument;
        OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes);
        b->complete(); b->release();
        return kIOReturnSuccess;
    }
    IOReturn rmCall(const OSSymbol *fn, void *p1) {
        if (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES)) {
            NVRMRmHandles *h = (NVRMRmHandles *)p1;
            if (!h || h->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
            h->kapiDevice = this; h->hClient = 0x50480001; h->hDevice = 0x50480002; h->hSubDevice = 0x50480003;
            return kIOReturnSuccess;
        }
        NVRMGpuVaRequest *g = (NVRMGpuVaRequest *)p1;
        if (!g || g->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
        if (fn->isEqualTo(NVRM_GPUVA_FN_ALLOC)) {
            const uint64_t len = round_page_64(g->size);
            uint64_t align = g->align > PAGE_SIZE ? g->align : PAGE_SIZE;
            if (align & (align - 1)) align = PAGE_SIZE;
            IORangeScalar off = 0;
            if (!fVa || !len || !fVa->allocate(len, &off, align)) { g->status = 1; return kIOReturnNoSpace; }
            g->gpuva = kVaBase + off; g->hVirt = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_MAP)) {   // nothing to map: no GPU work reaches these surfaces
            g->hMemory = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_FREE)) {
            if (!(g->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY) && fVa && g->gpuva >= kVaBase && g->gpuva - kVaBase < kVaSize)
                fVa->deallocate(g->gpuva - kVaBase, round_page_64(g->size));
            g->status = 0;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }

    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        PhxAMDFB *me = (PhxAMDFB *)p0;
        if (me->fStopping) return;
        const uint64_t at0 = mach_absolute_time(), due = me->fVblNext;
        uint64_t near; nanoseconds_to_absolutetime(50000, &near);
        const int spinUs = gVblStaged ? (gVblSpinUs < 0 ? 0 : gVblSpinUs > 1000 ? 1000 : gVblSpinUs) : 0;
        uint64_t at = at0;
        if (spinUs) {
            uint64_t spin; nanoseconds_to_absolutetime((uint64_t)spinUs * 1000, &spin);
            if (due > at + spin + near) { thread_call_enter_delayed(me->fVblTimer, due - spin); return; }   // an early stage
            while (at < due) { __builtin_ia32_pause(); at = mach_absolute_time(); }                     // the last stretch
        } else if (gVblStaged && due > at + near) { thread_call_enter_delayed(me->fVblTimer, due); return; }   // an early stage
        if (due) {
            uint64_t ns; absolutetime_to_nanoseconds(at > due ? at - due : due - at, &ns);
            if ((SInt64)(ns / 1000) > gVblJitterUsMax) gVblJitterUsMax = (SInt64)(ns / 1000);
        }
        if (due && at > due) {
            uint64_t ns; absolutetime_to_nanoseconds(at - due, &ns);
            const SInt64 us = (SInt64)(ns / 1000);
            OSAddAtomic64(us, &gVblLateUsSum);
            if (us > gVblLateUsMax) gVblLateUsMax = us;
        }
        OSAddAtomic64(1, &gVblN);
        gPhxVblLastAbs = due ? due : at;   // the vblank this tick stands for
        __atomic_add_fetch(&gPhxVblSeq, 1, __ATOMIC_RELEASE);
        for (const Irq &q : me->fVbl) {
            const IOFBInterruptProc proc = __atomic_load_n(&q.proc, __ATOMIC_ACQUIRE);
            if (proc) proc(q.target, q.ref);
        }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        const uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        uint64_t lead; nanoseconds_to_absolutetime(1500000, &lead);
        thread_call_enter_delayed(me->fVblTimer, gVblStaged ? me->fVblNext - lead : me->fVblNext);
    }

    // The firmware lit the panel at its native refresh, which the VBL timer and the mode must report: they said 60 Hz while
    // the panel scanned at 120 (OTG frame counter: 392 frames in 3.28 s), and WindowServer composited at 60 fps (2026-10-20).
    // Read-only: OTG0's H/V totals (DCN 3.1.4: segment 2 base 0x34c0 + 0x1b2a / 0x1b2f) and its frame counter (+0x1b4c)
    // over ~200 ms, snapped to a standard rate when within 2 %.
    void measureRefresh() {
        IOMemoryMap *mm = fPCI ? fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
        if (!mm || mm->getLength() < 0x5100u * 4) { OSSafeReleaseNULL(mm); PFBLOG("refresh: no MMIO view, 60 Hz assumed"); return; }
        volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
        const uint32_t ctl = r[0x34c0 + 0x1b41], ht = r[0x34c0 + 0x1b2a] + 1, vt = r[0x34c0 + 0x1b2f] + 1;
        const uint32_t c0 = r[0x34c0 + 0x1b4c] & 0xffffff; const uint64_t t0 = mach_absolute_time();
        IOSleep(200);
        const uint32_t c1 = r[0x34c0 + 0x1b4c] & 0xffffff; const uint64_t t1 = mach_absolute_time();
        mm->release();
        uint64_t ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
        const uint32_t frames = (c1 - c0) & 0xffffff;
        if (!(ctl & 1) || !ns || frames < 4 || ht < 100 || vt < 100) { PFBLOG("refresh: OTG0 not running (ctl 0x%x, %u frames), 60 Hz assumed", ctl, frames); return; }
        uint32_t mhz = (uint32_t)(((uint64_t)frames * 1000000000000ull) / ns);
        static const uint32_t kStd[] = { 48000, 50000, 60000, 75000, 90000, 100000, 120000, 144000, 165000, 240000 };
        for (uint32_t k : kStd) if (mhz > k * 49 / 50 && mhz < k * 51 / 50) { mhz = k; break; }
        if (mhz < 24000 || mhz > 360000) { PFBLOG("refresh: %u mHz measured, out of range - 60 Hz assumed", mhz); return; }
        fRefreshMilliHz = mhz; fHTotal = ht; fVTotal = vt;
        fFrameNs = 1000000000000ull / mhz;
        PFBLOG("refresh: OTG0 %ux%u totals, %u frames in %llu us -> %u.%03u Hz", ht, vt, frames, ns / 1000, mhz / 1000, mhz % 1000);
    }

public:
    // the backlight as 0..kBlMax of full duty; false without a running PWM
    bool backlightRead(uint32_t *level) {
        if (!fMmio || !level) return false;
        volatile uint32_t *r = (volatile uint32_t *)fMmio->getVirtualAddress();
        const uint32_t pc = r[kBlPeriod], period = pc & 0xffff, bits = (pc >> 16) & 0xf, cntl = r[kBlCntl];
        const uint64_t full = bits ? ((uint64_t)0x10000 * period) >> bits : 0;
        if (!full || !(cntl >> 31)) return false;
        const uint64_t l = (uint64_t)(cntl & 0xffff) * kBlMax / full;
        *level = l > kBlMax ? kBlMax : (uint32_t)l;
        return true;
    }
    // level 0 is dark, as on a MacBook: AppleBacklightDisplay puts the panel to sleep with brightness 0
    bool backlightWrite(uint32_t level) {
        if (!fMmio) return false;
        volatile uint32_t *r = (volatile uint32_t *)fMmio->getVirtualAddress();
        if (level > kBlMax) level = kBlMax;
        uint64_t frac = (uint64_t)level * 0x10000 / kBlMax;   // 16.16
        if (level && frac < 0x10000 / 100) frac = 0x10000 / 100;   // above dark, at least 1 % of full duty
        bool any = false;
        for (uint32_t q : kBlSeqs) {
            const uint32_t pc = r[kBlPeriod + q], period = pc & 0xffff, bits = (pc >> 16) & 0xf;
            if (!period || !bits || !(r[kBlCntl + q] >> 31)) continue;
            const uint64_t duty = frac * period;
            uint32_t cnt = (uint32_t)((duty >> bits) & 0xffff) + (uint32_t)((duty >> (bits - 1)) & 1);
            if (cnt > 0xffff) cnt = 0xffff;
            r[kBlLock + q] = r[kBlLock + q] | 0x80000000u | 1u;                 // IGNORE_MASTER_LOCK_EN, REG_LOCK
            r[kBlCntl + q] = (r[kBlCntl + q] & ~0xffffu) | cnt;                 // BL_ACTIVE_INT_FRAC_CNT
            r[kBlLock + q] = (r[kBlLock + q] | 0x80000000u) & ~1u;
            for (int i = 0; i < 100 && (r[kBlLock + q] & 0x100); i++) IODelay(100);   // REG_UPDATE_PENDING
            any = true;
        }
        return any;
    }
    // Display sleep left the backlight lit over a black or torn frame (2026-10-25). Besides brightness 0, a connection
    // powered down goes dark and gets its level back on power-up; a brightness set while dark waits.
    bool fBlDark = false;
    uint32_t fBlSaved = kBlMax;
    bool backlightSet(uint32_t level) {
        if (fBlDark) { fBlSaved = level > kBlMax ? kBlMax : level; return true; }
        return backlightWrite(level);
    }
    void displayPower(bool on) {
        if (!fBacklight || on != fBlDark) return;
        if (!on && !(backlightRead(&fBlSaved) && backlightWrite(0))) return;
        fBlDark = !on;
        if (on) backlightWrite(fBlSaved);
        fBacklight->setProperty("PhxDark", fBlDark);
        PFBLOG("backlight: display power %s, level %u", on ? "on" : "off", fBlSaved);
    }
    bool backlightDark() const { return fBlDark; }
    // which calls reach the panel, newest first, as the registry property PhxTrace (the brightness slider moved and no
    // brightness parameter arrived, 2026-10-25); the log does not reach the unified log
    char fTrace[16][48] = {}; unsigned fTraceN = 0;
    void trace(const char *what, uint32_t a, uint64_t v) {
        char line[48]; snprintf(line, sizeof line, "%s %c%c%c%c=0x%llx", what, (char)(a >> 24), (char)(a >> 16), (char)(a >> 8), (char)a,
                                (unsigned long long)v);
        if (fTraceN && !strcmp(line, fTrace[(fTraceN - 1) % 16])) return;   // repeats of the last call
        strlcpy(fTrace[fTraceN++ % 16], line, sizeof fTrace[0]);
        char all[16 * 48 + 16] = ""; for (unsigned i = 0; i < 16 && i < fTraceN; i++) { strlcat(all, fTrace[(fTraceN - 1 - i) % 16], sizeof all); strlcat(all, "; ", sizeof all); }
        setProperty("PhxTrace", all);
    }
    // the built-in panel is a TFT: AppleBacklightDisplay then drives it (brightness, dimming), as on a MacBook
    IOReturn getAppleSense(IOIndex idx, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override {
        if (idx != 0 || !fBacklight) return kIOReturnUnsupported;
        if (senseType) *senseType = 0;
        if (primary) *primary = 0;
        if (extended) *extended = 0;
        if (displayType) *displayType = 2;   // kPanelTFTConnect (IOMacOSVideo.h, the NDRV headers)
        return kIOReturnSuccess;
    }
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return !fBeside; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight);
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (mode != 1 || !info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
        info->refreshRate = (IOFixed1616)(((uint64_t)fRefreshMilliHz << 16) / 1000); info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag | kFramebufferSupportsGammaCorrection;
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fRowBytes; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fWidth; pi->activeHeight = fHeight;
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    // Detailed timing (f7bsc patch, 2026-11-19). Without it IOFramebuffer's VBL handler publishes the measured interval
    // between our soft ticks as vblDelta (16.5-16.8 ms with tick latency), WindowServer hands that to CoreVideo
    // (SLSDisplayGetCurrentVBLDelta), and CVDisplayLink re-reads it as its period before every callback. Its video time
    // is an absolute frame count (~uptime / period) times that period, so a 0.5% wobble put CVDisplayLink's output
    // times seconds off; whenever the error landed within 4 periods it believed it and slept 2-4 frames (the 33-67 ms
    // gaps Chrome's rAF saw). With a valid pixel clock IOFramebuffer::setVBLTiming switches to its timing mode
    // (gIOFBVBLThrottle, on by default): vblDelta = h_total * v_total / pixel clock, fixed, and vblCount advances by
    // round(elapsed / interval). The clock is chosen so that interval is the tick's own period (fFrameNs): the panel
    // itself is never programmed from this (setDisplayMode accepts only mode 1, unchanged). Verified in user space by
    // forcing CoreVideo's delta to a constant: gaps 4-frame 1/12 s -> 0, output-time error seconds -> < 9 ms.
    // debug.phxfb_timing=0 before a display reconfiguration (or at boot via the LaunchDaemon) gives the old answer.
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;   // no Apple timing ID: the panel keeps the firmware's mode
        if (!gTiming || fHTotal <= fWidth || fVTotal <= fHeight || !fFrameNs) return kIOReturnSuccess;
        const uint64_t pixels = (uint64_t)fHTotal * fVTotal;
        const uint64_t clock = pixels * 1000000000ull / fFrameNs;   // Hz, rounded down: IOFramebuffer's truncated pixels/clock == fFrameNs
        IODetailedTimingInformationV2 &t = info->detailedInfo.v2;
        t.pixelClock = t.minPixelClock = t.maxPixelClock = clock;
        t.horizontalActive = fWidth;  t.horizontalBlanking = fHTotal - fWidth;
        t.verticalActive = fHeight;   t.verticalBlanking = fVTotal - fHeight;
        info->flags = kIODetailedTimingValid;
        if (!fTimingLogged) { fTimingLogged = true;
            PFBLOG("timing: %ux%u in %ux%u totals, pixel clock %llu Hz -> VBL interval %llu ns", fWidth, fHeight, fHTotal, fVTotal,
                   (unsigned long long)clock, (unsigned long long)(pixels * 1000000000ull / clock)); }
        return kIOReturnSuccess;
    }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) {
            for (Irq &q : fVbl) if (!q.proc) {
                q.target = target; q.ref = ref; __atomic_store_n(&q.proc, proc, __ATOMIC_RELEASE);
                PFBLOG("VBL handler %u: %s", (unsigned)(&q - fVbl), target ? target->getMetaClass()->getClassName() : "(no target)");
                if (iref) *iref = &q;
                return kIOReturnSuccess;
            }
            PFBLOG("VBL handler refused: all %u taken", (unsigned)(sizeof fVbl / sizeof fVbl[0]));
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        for (Irq &q : fVbl) if (iref == &q) __atomic_store_n(&q.proc, (IOFBInterruptProc)nullptr, __ATOMIC_RELEASE);
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // power and connection answers as NullMoth's NMIntelFB/NVRMFB give them: acknowledge at once, never queue on the
    // controller thread (a base-class power change waited 45 s for an acknowledge and froze WindowServer restarts)
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        if (ordinal) hdmiAviFull();
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        trace("attr", attribute, value);
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            displayPower(value != 0);
            if (value) { hdmiAviFull(); IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        trace("conn", attribute, value);
        switch (attribute) {
        case kConnectionPower:
            displayPower(value != 0);
            return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0 || !value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    // The accelerator's display pipe asks for the scan-out: its CPU mapping (frames are copied in) and its physical
    // address in the GPU's VRAM aperture. With no address the pipe got no framebuffer resource, and IOAcceleratorFamily
    // marks a display pipe active only once one exists (init_framebuffer_resource sets +0x298): every WindowServer
    // transaction ended kIOReturnNotReady and no frame reached the panel (2026-10-20).
    // The scan-out through the carve-out, once PhxBringup has published where the carve-out sits (phx-fb-phys) and only
    // if that view holds the pixels the BAR shows. NULL (copy through the BAR) until then.
    IOMemoryMap *aliasMap() {
        if (fAliasMap || fAliasTries >= 3) return fAliasMap;
        if (!OSCompareAndSwap(0, 1, &fAliasBusy)) return nullptr;
        if (!fAliasMap && fAliasTries < 3) {
            fAliasTries++;
            uint64_t fbPhys = 0, barOff = ~0ull;
            if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
                while (OSObject *o = it->getNextObject()) {
                    IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
                    OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty("phx-fb-phys")) : nullptr;
                    if (n) { fbPhys = n->unsigned64BitValue(); break; }
                }
                it->release();
            }
            for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
                IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
                if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                    barOff = fBase - dm->getPhysicalAddress();
                    break;
                }
            }
            const uint64_t phys = fbPhys + barOff, bytes = (uint64_t)fRowBytes * fHeight;
            IOMemoryMap *m = nullptr;
            if (fbPhys && barOff != ~0ull)
                if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { m = dm->map(kIOMapWriteCombineCache); dm->release(); }
            if (m) {
                const volatile uint32_t *a = (const volatile uint32_t *)m->getVirtualAddress();
                const volatile uint32_t *b = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
                const uint64_t words = bytes / 4, step = words / 4096 ? words / 4096 : 1;
                unsigned same = 0, nz = 0, n = 0;
                for (uint64_t w = 0; w < words; w += step, n++) { const uint32_t x = a[w], y = b[w]; same += x == y; nz += y != 0; }
                PFBLOG("alias: scan-out at BAR offset 0x%llx = system RAM 0x%llx: %u of %u samples match the BAR (%u non-zero)",
                       barOff, phys, same, n, nz);
                if (same == n && nz) fAliasMap = m; else m->release();
            } else {
                PFBLOG("alias: carve-out 0x%llx, scan-out BAR offset 0x%llx: no system-RAM view, copying through the BAR", fbPhys, barOff);
            }
        }
        fAliasBusy = 0;
        return fAliasMap;
    }

    // ---- double-buffered scan-out (see debug.phxfb_dbuf) ----
    // DCN 3.1.4 registers, dword index in BAR5 (segment 2 base 0x34c0; Linux dcn_3_1_4_offset.h / sh_mask.h)
    enum : uint32_t {
        kSeg2 = 0x34c0,
        kHubpTiling = kSeg2 + 0x05e7,        // HUBP0_DCSURF_TILING_CONFIG, SW_MODE 4:0 (0 = linear)
        kHubpPitch = kSeg2 + 0x0607,         // HUBPREQ0_DCSURF_SURFACE_PITCH, PITCH 13:0 (pixels)
        kHubpAddrLo = kSeg2 + 0x060a,        // HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS (the write that arms the flip)
        kHubpAddrHi = kSeg2 + 0x060b,        // ..._HIGH, 15:0
        kHubpFlipCtl = kSeg2 + 0x061b,       // HUBPREQ0_DCSURF_FLIP_CONTROL: 0 UPDATE_LOCK, 1 FLIP_TYPE (1 = immediate), 8 FLIP_PENDING
        kHubpInuseLo = kSeg2 + 0x0621, kHubpInuseHi = kSeg2 + 0x0622,   // the address DCN is reading now
        kHubpSysLo = kSeg2 + 0x0635, kHubpSysHi = kSeg2 + 0x0636,       // HUBPREQ0 system aperture, 256 KiB units
        kVmFbBase = kSeg2 + 0x0475, kVmFbTop = kSeg2 + 0x0476,          // DCN_VM_FB_LOCATION_BASE/TOP, 16 MiB units
    };
    IOLock *fDbufLock = nullptr;
    IOMemoryMap *fBackMap = nullptr;   // the second surface: carve-out system-RAM view, written with non-temporal stores
    uint64_t fMc[2] = { 0, 0 };        // DCN addresses of surface 0 (the firmware's) and surface 1
    void *fKvaBuf[2] = { nullptr, nullptr };
    uint64_t fFrameBytes = 0;
    unsigned fFront = 0;               // the surface HUBP0 was last pointed at
    bool fDbufOn = false, fDbufHeld = false;
    SInt32 fDbufGenSeen = -1;

    uint32_t mmio(uint32_t i) { return ((volatile uint32_t *)fMmio->getVirtualAddress())[i]; }
    void mmioW(uint32_t i, uint32_t v) { ((volatile uint32_t *)fMmio->getVirtualAddress())[i] = v; }
    // ---- HDMI AVI InfoFrame (f7bsc patch) ----
    // GOP lights HDMI without any InfoFrame (HDMI_GENERIC_PACKET_CONTROL0 SEND/CONT all 0). Sinks then pick the RGB
    // quantization range themselves, and some take limited range: 0-16 and 235-255 crush to black / white, and the light
    // borders and shadows of the UI vanish. Send the AVI InfoFrame amdgpu sends (RGB, Q = full range, VIC 0) the way
    // enc3_update_hdmi_info_packet / vpg3_update_generic_info_packet do: VPG generic packet 0 of the stream encoder that
    // feeds the HDMI back end, sent every frame on line 2. Boot-arg -phxavioff leaves the link as GOP set it.
    // DIG n / VPG n (n = 0..4) are 0x100 apart; offsets are DIG0 / VPG0's (Linux dcn_3_1_4_offset.h, sh_mask.h).
    enum : uint32_t {
        kDigBeCntl = kSeg2 + 0x20b1,         // DIG_BE_CNTL: FE_SOURCE_SELECT 14:8 (one bit per FE), DIG_MODE 18:16 (3 = HDMI)
        kHdmiGenCtl0 = kSeg2 + 0x209b,       // HDMI_GENERIC_PACKET_CONTROL0 (of the FE): GENERIC0_SEND 0, GENERIC0_CONT 1
        kHdmiGenCtl1 = kSeg2 + 0x209f,       // HDMI_GENERIC_PACKET_CONTROL1: GENERIC0_LINE 15:0
        kHdmiDbCtl = kSeg2 + 0x20a7,         // HDMI_DB_CONTROL: HDMI_DB_DISABLE 12
        kVpgAccess = kSeg2 + 0x2068,         // VPG_GENERIC_PACKET_ACCESS_CTRL: DATA_INDEX 7:0 (packet n = dwords 9n..9n+8)
        kVpgData = kSeg2 + 0x2069,           // VPG_GENERIC_PACKET_DATA
        kVpgImmUpd = kSeg2 + 0x206b,         // VPG_GSP_IMMEDIATE_UPDATE_CTRL: GENERIC0_IMMEDIATE_UPDATE 0
        kVpgStatus = kSeg2 + 0x206c,         // VPG_GENERIC_STATUS: CONFLICT_CLR 4
        kVpgMemPwr = kSeg2 + 0x206d,         // VPG_MEM_PWR: GSP_MEM_LIGHT_SLEEP_DIS 0, GSP_LIGHT_SLEEP_FORCE 4
    };
    int fAviState = 0;                       // 1 sent, 0 not tried, -1 no MMIO, -2 no HDMI stream, -3 -phxavioff
    void hdmiAviFull() {
        char arg[8];
        if (PE_parse_boot_argn("-phxavioff", arg, sizeof arg)) { fAviState = -3; return; }
        if (!fMmio || fMmio->getLength() < (kDigBeCntl + 4 * 0x100 + 1) * 4) { fAviState = -1; return; }
        int fe = -1;
        for (uint32_t be = 0; be < 5 && fe < 0; be++) {
            const uint32_t c = mmio(kDigBeCntl + be * 0x100), sel = (c >> 8) & 0x7f;
            if (((c >> 16) & 7) == 3 && sel) fe = __builtin_ctz(sel);
        }
        if (fe < 0 || fe > 4) { fAviState = -2; setProperty("PhxHdmiAvi", "no HDMI stream"); return; }
        const uint32_t o = (uint32_t)fe * 0x100;
        // HB0 0x82 (AVI) HB1 2 (version) HB2 13 (length); PB0 checksum, PB1 0 (RGB), PB2 0x08 (aspect as picture),
        // PB3 0x08 (Q = 2, full range); the rest 0. Checksum: all header and payload bytes sum to 0 mod 256.
        const uint8_t pb1 = 0x00, pb2 = 0x08, pb3 = 0x08;
        const uint8_t sum = (uint8_t)(0x100 - ((0x82 + 0x02 + 0x0d + pb1 + pb2 + pb3) & 0xff));
        const uint32_t words[9] = { 0x000d0282u, (uint32_t)sum | (uint32_t)pb1 << 8 | (uint32_t)pb2 << 16 | (uint32_t)pb3 << 24,
                                    0, 0, 0, 0, 0, 0, 0 };
        mmioW(kVpgMemPwr + o, (mmio(kVpgMemPwr + o) & ~0x10u) | 0x1u);   // GSP memory out of light sleep (vpg31_poweron)
        mmioW(kVpgStatus + o, mmio(kVpgStatus + o) | 0x10u);              // clear a stale conflict flag
        for (uint32_t i = 0; i < 9; i++) { mmioW(kVpgAccess + o, i); mmioW(kVpgData + o, words[i]); }
        mmioW(kVpgImmUpd + o, mmio(kVpgImmUpd + o) | 0x1u);
        mmioW(kHdmiDbCtl + o, mmio(kHdmiDbCtl + o) | 0x1000u);
        mmioW(kHdmiGenCtl1 + o, (mmio(kHdmiGenCtl1 + o) & ~0xffffu) | 2u);
        mmioW(kHdmiGenCtl0 + o, mmio(kHdmiGenCtl0 + o) | 0x3u);
        if (fAviState != 1) PFBLOG("HDMI: AVI InfoFrame (RGB full range) on DIG FE %d", fe);
        fAviState = 1;
        char s[40]; snprintf(s, sizeof s, "RGB full range, FE %d", fe); setProperty("PhxHdmiAvi", s);
    }
    uint64_t hubpAddr() { return ((uint64_t)(mmio(kHubpAddrHi) & 0xffff) << 32) | mmio(kHubpAddrLo); }
    bool flipPending() { return (mmio(kHubpFlipCtl) & 0x100u) != 0; }
    uint64_t childProp(const char *name) {
        uint64_t v = 0;
        if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject()) {
                IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
                OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty(name)) : nullptr;
                if (n) { v = n->unsigned64BitValue(); break; }
            }
            it->release();
        }
        return v;
    }
    // waits for the last flip to latch; false when it did not within ~2.5 refreshes
    bool waitFlipLatched() {
        if (!flipPending()) return true;
        const uint64_t t0 = mach_absolute_time();
        uint64_t limit; nanoseconds_to_absolutetime(fFrameNs * 5 / 2, &limit);
        bool ok = false;
        while (mach_absolute_time() - t0 < limit) { if (!flipPending()) { ok = true; break; } IODelay(20); }
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        if ((int)(ns / 1000) > gDbufWaitMaxUs) gDbufWaitMaxUs = (int)(ns / 1000);
        OSAddAtomic64(1, ok ? &gDbufWaits : &gDbufWaitTimeouts);
        return ok;
    }
    void hubpPoint(unsigned idx) {
        uint32_t fc = mmio(kHubpFlipCtl);
        if (fc & 0x2u) mmioW(kHubpFlipCtl, fc & ~0x2u);   // flip at VUPDATE, never immediately (an immediate flip tears)
        mmioW(kHubpAddrHi, (uint32_t)(fMc[idx] >> 32) & 0xffff);   // high first: the low write arms the flip (hubp2_program_surface_flip_and_addr)
        mmioW(kHubpAddrLo, (uint32_t)fMc[idx]);
        fFront = idx;
    }
    // lock held. Back to the firmware's surface with the newest frame in it, so a reader of the aperture (or the
    // module going away) sees what the panel showed.
    void dbufHome(const char *why) {
        if (!fDbufOn) return;
        fDbufOn = false;
        if (fMmio && fFront != 0 && hubpAddr() == fMc[1]) {
            waitFlipLatched();
            phxNtCopy(fKvaBuf[0], fKvaBuf[1], fFrameBytes);
            hubpPoint(0);
            waitFlipLatched();
        }
        fFront = 0;
        OSAddAtomic64(1, &gDbufHomes);
        if (gDbufState == 1) gDbufState = 0;
        PFBLOG("dbuf: back on the firmware's surface (%s)", why);
    }
    void dbufRefuse(int state, const char *fmt, uint64_t a, uint64_t b) {
        gDbufState = state;
        PFBLOG("dbuf: refused (%d): %s 0x%llx 0x%llx", state, fmt, (unsigned long long)a, (unsigned long long)b);
    }
    // lock held. Checks everything against the running display engine, then maps the second surface.
    bool dbufSetup() {
        if (!fMmio || fMmio->getLength() < (uint64_t)(kHubpSysHi + 1) * 4) { dbufRefuse(-1, "no BAR5 view", 0, 0); return false; }
        IOMemoryMap *alias = gAlias ? aliasMap() : nullptr;
        const uint64_t fbPhys = childProp("phx-fb-phys"), fbSize = childProp("phx-fb-size"), mcBase = childProp("phx-fb-mc-base"),
                       aperLen = childProp("phx-fb-aper-len");
        if (!alias || !fbPhys || !fbSize || !mcBase || !aperLen) { dbufRefuse(-2, "carve-out phys/size or the alias view missing", fbPhys, fbSize); return false; }
        uint64_t barOff = ~0ull;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) { barOff = fBase - dm->getPhysicalAddress(); break; }
        }
        const uint64_t cur = hubpAddr();
        if (barOff == ~0ull || cur != mcBase + barOff) { dbufRefuse(-3, "HUBP0 scans out / expected", cur, mcBase + barOff); return false; }
        { const uint32_t p = mmio(kHubpPitch) & 0x3fff;   // Linux writes pixels - 1 (hubp2_program_size)
        if ((p + 1 != fRowBytes / 4 && p != fRowBytes / 4) || (fRowBytes & 3)) { dbufRefuse(-4, "HUBP0 pitch / row pixels", mmio(kHubpPitch) & 0x3fff, fRowBytes / 4); return false; } }
        if (mmio(kHubpTiling) & 0x1f) { dbufRefuse(-11, "HUBP0 surface is not linear, SW_MODE", mmio(kHubpTiling) & 0x1f, 0); return false; }
        if (mmio(kHubpFlipCtl) & 0x1u) { dbufRefuse(-8, "SURFACE_UPDATE_LOCK is held, FLIP_CONTROL", mmio(kHubpFlipCtl), 0); return false; }
        const uint64_t bytes = round_page_64((uint64_t)fRowBytes * fHeight);
        const uint64_t off = ((uint64_t)(gDbufOffMb > 0 ? gDbufOffMb : 512) << 20);
        if (off < aperLen || off < barOff + bytes || off + bytes > fbSize) { dbufRefuse(-9, "second surface offset / carve-out size", off, fbSize); return false; }
        const uint64_t mc1 = mcBase + off;
        const uint64_t fbLo = (uint64_t)(mmio(kVmFbBase) & 0xffffff) << 24, fbHi = ((uint64_t)(mmio(kVmFbTop) & 0xffffff) << 24) | 0xffffff;
        if (cur < fbLo || cur + bytes - 1 > fbHi || mc1 < fbLo || mc1 + bytes - 1 > fbHi) { dbufRefuse(-5, "outside DCN's FB window", fbLo, fbHi); return false; }
        const uint64_t sLo = (uint64_t)(mmio(kHubpSysLo) & 0x3fffffff) << 18, sHi = ((uint64_t)(mmio(kHubpSysHi) & 0x3fffffff) << 18) | 0x3ffff;
        const bool curSys = cur >= sLo && cur + bytes - 1 <= sHi, newSys = mc1 >= sLo && mc1 + bytes - 1 <= sHi;
        if (curSys != newSys) { dbufRefuse(-6, "system aperture covers one surface only", sLo, sHi); return false; }
        if (!fBackMap) {
            if (IODeviceMemory *dm = IODeviceMemory::withRange(fbPhys + off, bytes)) { fBackMap = dm->map(kIOMapWriteCombineCache); dm->release(); }
            if (!fBackMap) { dbufRefuse(-7, "second surface would not map at", fbPhys + off, bytes); return false; }
        }
        fMc[0] = cur; fMc[1] = mc1; fFrameBytes = bytes;
        fKvaBuf[0] = (void *)alias->getVirtualAddress(); fKvaBuf[1] = (void *)fBackMap->getVirtualAddress();
        phxNtCopy(fKvaBuf[1], fKvaBuf[0], bytes);   // the first flip shows the picture already on the panel
        fFront = 0; fDbufOn = true; gDbufState = 1;
        PFBLOG("dbuf: on: surface 0 at DCN 0x%llx, surface 1 at DCN 0x%llx (carve-out +0x%llx, phys 0x%llx), %llu bytes, FB window 0x%llx-0x%llx",
               (unsigned long long)fMc[0], (unsigned long long)fMc[1], (unsigned long long)off, (unsigned long long)(fbPhys + off),
               (unsigned long long)bytes, (unsigned long long)fbLo, (unsigned long long)fbHi);
        return true;
    }
    // The display pipe asks for the surface to copy the next frame into. On success the lock stays held until
    // phxScanoutFlip (with the request, to show it; with NULL, to abandon it).
    IOReturn dbufBack(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (!fDbufLock || fStopping) return kIOReturnUnsupported;
        if (!gDbuf && !fDbufOn) return kIOReturnUnsupported;   // the common off case: no lock, no register reads
        IOLockLock(fDbufLock);
        const SInt32 gen = gDbufGen;
        if (gDbuf > 1 && gDbufTrialEnd && mach_absolute_time() > gDbufTrialEnd) { gDbuf = 0; gDbufTrialEnd = 0; PFBLOG("dbuf: trial over"); }
        if (!gDbuf) { dbufHome("debug.phxfb_dbuf=0"); IOLockUnlock(fDbufLock); return kIOReturnUnsupported; }
        if (!fDbufOn) {
            if (gDbufState < 0 && gen == fDbufGenSeen) { IOLockUnlock(fDbufLock); return kIOReturnUnsupported; }   // refused: wait for a new sysctl write
            fDbufGenSeen = gen;
            if (!dbufSetup()) { IOLockUnlock(fDbufLock); return kIOReturnUnsupported; }
        }
        // the panel must still be scanning one of ours. After a display sleep or anything else that re-programs HUBP0,
        // follow it when it points at one of the two; otherwise let go and touch nothing.
        const uint64_t cur = hubpAddr();
        if (cur != fMc[fFront]) {
            if (cur == fMc[0] || cur == fMc[1]) { fFront = cur == fMc[1] ? 1 : 0; OSAddAtomic64(1, &gDbufAdopted); }
            else { fDbufOn = false; dbufRefuse(-10, "HUBP0 re-programmed, scans out / ours", cur, fMc[fFront]); IOLockUnlock(fDbufLock); return kIOReturnUnsupported; }
        }
        // the surface not on the panel may still be read by DCN until the previous flip has latched
        if (!waitFlipLatched()) { IOLockUnlock(fDbufLock); return kIOReturnBusy; }
        const unsigned back = fFront ^ 1u;
        r->kva = fKvaBuf[back]; r->phys = 0; r->actualSize = fFrameBytes;
        r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes; r->flags = back;
        fDbufHeld = true;
        return kIOReturnSuccess;   // lock held
    }
    IOReturn dbufFlip(NVRMVramRequest *r) {
        if (!fDbufLock || !fDbufHeld) return kIOReturnNotPermitted;
        fDbufHeld = false;
        if (r && r->version == NVRM_VRAM_ABI_VERSION && fDbufOn && r->flags < 2 && r->flags != fFront) {
            __asm__ __volatile__("sfence" ::: "memory");
            hubpPoint(r->flags);
            OSAddAtomic64(1, &gDbufFlips);
        }
        IOLockUnlock(fDbufLock);
        return kIOReturnSuccess;
    }
    void dbufStop() {
        if (!fDbufLock) return;
        IOLockLock(fDbufLock);
        dbufHome("stop");
        OSSafeReleaseNULL(fBackMap);
        IOLockUnlock(fDbufLock);
    }

    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        if (fn && fn->isEqualTo(PHX_FN_SCANOUT_BACK)) return dbufBack((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(PHX_FN_SCANOUT_FLIP)) return dbufFlip((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
            NVRMVramRequest *r = (NVRMVramRequest *)p1;
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
            if (!fSurfMap) return kIOReturnNotReady;
            IOMemoryMap *alias = gAlias ? aliasMap() : nullptr;
            r->kva = (void *)(alias ? alias : fSurfMap)->getVirtualAddress();
            r->phys = fBase; r->actualSize = round_page_64((uint64_t)fRowBytes * fHeight);
            r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
            return kIOReturnSuccess;
        }
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_ALLOC)) return vramAlloc((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_FREE)) return vramFree((NVRMVramRequest *)p1);
        if (fn && (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES) || fn->isEqualTo(NVRM_GPUVA_FN_ALLOC) || fn->isEqualTo(NVRM_GPUVA_FN_MAP) ||
                   fn->isEqualTo(NVRM_GPUVA_FN_FREE)))
            return rmCall(fn, p1);
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
};

OSDefineMetaClassAndStructors(PhxAMDFB, IOFramebuffer)

// The panel's brightness as a display parameter. IOGraphics looks for parameter handlers under the framebuffer, and
// takes a built-in TFT panel for a backlit one (AppleBacklightDisplay) only while a service named "backlight" exists.
class PhxBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(PhxBacklight)
    PhxAMDFB *fFB = nullptr;
    uint32_t fLevel = kBlMax;
public:
    static PhxBacklight *withFramebuffer(PhxAMDFB *fb) {
        PhxBacklight *b = new PhxBacklight;
        if (b && !b->init()) OSSafeReleaseNULL(b);
        if (b) { b->fFB = fb; fb->backlightRead(&b->fLevel); }
        return b;
    }
    bool setDisplay(IODisplay *display) override {
        if (!display) return true;
        OSObject *p = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, p);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        OSSafeReleaseNULL(p);
        if (!params) return false;
        fFB->backlightRead(&fLevel);
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, kBlMax);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, (SInt32)fLevel);
        // CoreBrightness sets the linear ones, as an Intel MacBook's framebuffer offers them: with "brightness" alone
        // the slider moved and no parameter ever reached the panel (2026-10-25)
        const OSSymbol *linear[] = { gIODisplayLinearBrightnessKey, gIODisplayUsableLinearBrightnessKey };
        for (const OSSymbol *k : linear) { IODisplay::addParameter(params, k, 0, kBlMax); IODisplay::setParameter(params, k, (SInt32)fLevel); }
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        PFBLOG("backlight: brightness parameter on %s, %u of %u", display->getName(), fLevel, kBlMax);
        return true;
    }
    // the last parameter set, in the registry: PhxAMDFB's log does not reach the unified log
    uint32_t fSets = 0;
    bool doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value) override {
        setProperty("PhxLastParam", const_cast<OSSymbol *>(paramName)); setProperty("PhxLastValue", value, 32); setProperty("PhxSets", ++fSets, 32);
        fFB->trace(paramName->getCStringNoCopy(), 0x20202020, value);
        setProperty("PhxDark", fFB->backlightDark());
        if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey
            && paramName != gIODisplayUsableLinearBrightnessKey) return false;
        if (!fFB->backlightSet(value)) return false;
        fLevel = value > kBlMax ? kBlMax : value;
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *d = OSDynamicCast(OSDictionary, properties);
        if (OSCollectionIterator *it = d ? OSCollectionIterator::withCollection(d) : nullptr) {
            while (const OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) fFB->trace(k->getCStringNoCopy(), 0x2a2a2a2a, 0);
            it->release();
        }
        return kIOReturnUnsupported;
    }
    bool doUpdate() override { return true; }
};
OSDefineMetaClassAndStructors(PhxBacklight, IODisplayParameterHandler)

static IOService *phxBacklightCreate(PhxAMDFB *fb)
{
    PhxBacklight *b = PhxBacklight::withFramebuffer(fb);
    if (!b) return nullptr;
    b->setName("backlight");
    if (!b->attach(fb)) { b->release(); return nullptr; }
    b->registerService();
    return b;
}

// One boot per arming: `nvram phxfb-boot=1` lets the next start take the panel, and start removes the variable and
// commits NVRAM at once, so a boot that panics after this point comes back on the firmware framebuffer without hands.
static bool takeBootArming()
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty("phxfb-boot");
    const bool armed = o != nullptr;
    bool stuck = false;
    OSSafeReleaseNULL(o);
    if (armed) {
        opts->removeProperty("phxfb-boot");
        // the forced commit: the throttled one (IONVRAM-SYNCNOW) left the variable in flash once, and the boot after a
        // panic took the panel again (2026-10-20)
        if (OSString *why = OSString::withCString("phxfb")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
        o = opts->copyProperty("phxfb-boot");
        stuck = o != nullptr;
        PFBLOG("NVRAM phxfb-boot taken (%s)", stuck ? "STILL SET: removal refused, not taking the panel" : "removed and committed");
        OSSafeReleaseNULL(o);
    }
    opts->release();
    return armed && !stuck;
}

bool PhxAMDFB::start(IOService *provider)
{
    char arg[8];
    if (PE_parse_boot_argn("-phxfboff", arg, sizeof arg)) { PFBLOG("-phxfboff: leaving the firmware framebuffer"); return false; }
    if (!takeBootArming()) { PFBLOG("NVRAM phxfb-boot not set: leaving the firmware framebuffer this boot"); return false; }
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject())
            if (o != this && OSDynamicCast(IOFramebuffer, o)) fBeside = true;
        it->release();
    }
    PE_Video v;
    bzero(&v, sizeof(v));
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32 || !v.v_width || !v.v_height ||
        v.v_rowBytes < v.v_width * 4 || v.v_width > 16384 || v.v_height > 16384) {
        PFBLOG("no usable boot console (base 0x%lx %lux%lu depth %lu row %lu): leaving the firmware framebuffer",
               v.v_baseAddr, v.v_width, v.v_height, v.v_depth, v.v_rowBytes);
        return false;
    }
    fWidth = (uint32_t)v.v_width; fHeight = (uint32_t)v.v_height; fRowBytes = (uint32_t)v.v_rowBytes;
    // once the video console is up, the platform expert hands out the console's kernel mapping, not the physical
    // address. The GOP surface sits in one of the GPU's BARs (VRAM aperture): find the BAR whose start holds the same
    // pixels as the console mapping. (A descriptor over the mapping would need prepare(): getPhysicalSegment on an
    // unwired one panicked, 2026-10-20.)
    uint64_t base = v.v_baseAddr;
    if (base >= 0xFFFF800000000000ull) {
        const volatile uint32_t *con = (const volatile uint32_t *)(uintptr_t)base;
        const uint32_t probeWords = 64u * 1024u / 4u;
        uint64_t found = 0;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount() && !found; i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (!dm || dm->getLength() < (IOByteCount)fRowBytes * fHeight) continue;
            IODeviceMemory *sub = IODeviceMemory::withSubRange(dm, 0, 64u * 1024u);
            IOMemoryMap *m = sub ? sub->map(kIOMapInhibitCache) : nullptr;
            OSSafeReleaseNULL(sub);
            if (!m) continue;
            const volatile uint32_t *bar = (const volatile uint32_t *)m->getVirtualAddress();
            uint32_t same = 0, nz = 0;
            for (uint32_t w = 0; w < probeWords; w++) { const uint32_t a = con[w], b = bar[w]; same += a == b; nz += a != 0; }
            PFBLOG("BAR index %u at 0x%llx: %u of %u words match the console (%u non-zero)", i,
                   (unsigned long long)dm->getPhysicalAddress(), same, probeWords, nz);
            if (same == probeWords && nz) found = dm->getPhysicalAddress();
            m->release();
        }
        if (!found) { PFBLOG("the console surface is in none of the GPU's BARs: leaving the firmware framebuffer"); return false; }
        base = found;
    }
    // early in boot the platform expert hands out the physical address with a flag in its low bits (0x820000001): taken
    // as is, every row WindowServer drew landed one byte off and the panel showed swapped colours (2026-10-20)
    base &= ~(uint64_t)PAGE_MASK;
    fBase = (IOPhysicalAddress)base;
    PFBLOG("console: %ux%u row %u depth %lu at physical 0x%llx", fWidth, fHeight, fRowBytes, v.v_depth, (unsigned long long)fBase);
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
    }
    if (!fSurfMap) { PFBLOG("the console surface at 0x%llx would not map write-combined", (unsigned long long)fBase); return false; }
    // IOFramebuffer::start makes the framebuffer's controller only from a dependent ID that is already set
    // (IOFBController::copyController(kDepIDCreate)); without one the controller stays NULL and the first power-change
    // callout dereferences it (IOFramebuffer::powerStateWillChangeTo, panic 2026-10-20)
    setProperty("IOFBDependentID", 0x50485846ull << 32 | 1, 64);   // 'PHXF'
    setProperty("IOFBDependentIndex", 0ull, 32);
    // the backlight's handler comes first: IOGraphics checks for a "backlight" service once, when the framebuffer opens
    fMmio = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    fDbufLock = IOLockAlloc();
    if (fMmio && fMmio->getLength() < (kBlLock + kBlSeqs[1] + 1) * 4) OSSafeReleaseNULL(fMmio);
    uint32_t bl = 0;
    if (PE_parse_boot_argn("-phxbloff", arg, sizeof arg)) PFBLOG("backlight: -phxbloff, no brightness control");
    else if (!backlightRead(&bl)) PFBLOG("backlight: the PWM is not running (no MMIO view, or off), no brightness control");
    else if (!(fBacklight = phxBacklightCreate(this))) PFBLOG("backlight: the handler would not attach");
    else PFBLOG("backlight: PWM at %u of %u", bl, kBlMax);
    // before IOFramebuffer::start: its setup may ask for the mode's timing (getTimingInfoForDisplayMode needs the totals)
    measureRefresh();
    if (!IOFramebuffer::start(provider)) {
        if (fBacklight) { fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
        OSSafeReleaseNULL(fMmio); OSSafeReleaseNULL(fSurfMap); return false; }
    fVa = IORangeAllocator::withRange(kVaSize - 1, PAGE_SIZE, 0, IORangeAllocator::kLocking);
    poolInit();
    hdmiAviFull();
    setProperty("built-in", kOSBooleanTrue);
    setProperty("IOFBGammaWidth", 8ull, 32);
    setProperty("IOFBGammaCount", 256ull, 32);
    setProperty("DisplayGammaChannels", 3ull, 32);
    setProperty("DisplayGammaEntryCount", 256ull, 32);
    setProperty("DisplayGammaEntrySize", 1ull, 32);
    setProperty("PhxFBWidth", fWidth, 32); setProperty("PhxFBHeight", fHeight, 32);   // the AGDC's panel timing reads these
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }   // the AGDC's panel timing
    nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);
    fVblTimer = thread_call_allocate(vblFire, this);
    if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
    static IOPMPowerState states[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    registerPowerDriver(this, states, 2);
    phxHoldInit(this, kBootHoldSec);
    phxAgdcInstall();
    if (!gAliasSysctl) { for (auto *o : kFbOids) sysctl_register_oid(o); gAliasSysctl = true; }
    PFBLOG("panel published: %ux%u row %u at 0x%llx (vbl timer %s)", fWidth, fHeight, fRowBytes,
           (unsigned long long)fBase, fVblTimer ? "running" : "MISSING");
    return true;
}

void PhxAMDFB::stop(IOService *provider)
{
    fStopping = true;
    dbufStop();   // the panel back on the firmware's surface before its mappings go
    phxAgdcRemove();
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    if (gAliasSysctl) { for (auto *o : kFbOids) sysctl_unregister_oid(o); gAliasSysctl = false; }
    gPhxFrameAbs = 0;
    if (fBacklight) { fBacklight->terminate(); OSSafeReleaseNULL(fBacklight); }
    OSSafeReleaseNULL(fMmio);
    OSSafeReleaseNULL(fAliasMap);
    OSSafeReleaseNULL(fSurfMap);
    OSSafeReleaseNULL(fVa);
    poolFree();
    if (fDbufLock) { IOLockFree(fDbufLock); fDbufLock = nullptr; }
    IOFramebuffer::stop(provider);
}
