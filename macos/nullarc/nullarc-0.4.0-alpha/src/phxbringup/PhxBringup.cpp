//
//  PhxBringup.cpp — Phoenix (Radeon 780M, 1002:1900) bring-up kext, step E1.
//
//  Publishes the service the RADV Darwin backend opens (class PhxBringup, user client type 'N48N')
//  and answers ABI v1 selectors 0..8 with the same rules as Mesa's in-process mock kext
//  (src/amd/common/darwin/ac_darwin_mock.c): buffers are kernel/user shared system memory,
//  GPU VA mappings are bookkeeping only, and Submit retires at once without touching the GPU.
//  E1 proves the IOKit transport end to end; E3/E4 put the GPU behind the same calls.
//
//  Boot-arg -phxbringupoff disables it. ReadRegs (MMIO read) is root-only; nothing else touches hardware.
//

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "PhxNativeABI.h"
#include "PhxVcn.h"
#include "PhxVcnDecode.h"
#include "PhxVcnAperture.h"
#include "PhxVcnDecodeTestVector.h"
#include "../../tools/avc/avc_stream_parser.h"
#include "PhxVcnEncode.h"
#include "PhxVcnEncodeFrame.h"
#include "PhxVcnWait.h"
#include "../phxaccel/nvrm_vram_abi.h"
#include "../phxaccel/PhxVideoAVC.h"

using namespace phx_vcn;   // hard-coded VCN 4.0.2 constants (PhxVcn.h); the k* names below are all VCN's

#define PHX_KEXT_BUILD 82u

// debug.phxbos_flush: how a client's BOs are made coherent around each submission and each completed wait.
// 2 = clflush every line (the bring-up way), 1 = clflushopt (the same lines, pipelined), 0 = none (the GPU maps system
// memory snooped, kPteSysFlags). With clflush, WindowServer spent ~73% of its CPU here and composited ~13 fps; with
// none, 41 fps and no artifacts (2026-10-20), hence the default.
static int gBosFlush = 0;
static volatile SInt64 gBosFlushBytes;
SYSCTL_INT(_debug, OID_AUTO, phxbos_flush, CTLFLAG_RW | CTLFLAG_LOCKED, &gBosFlush, 0,
           "BO cache maintenance per submit/wait: 0 none, 1 clflushopt, 2 clflush");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushBytes,
            "bytes of BOs flushed from the CPU caches");
// debug.phx_hang_ms: a queue holding work whose fence has not moved for this long gets the waves of its oldest
// submission's VMID killed (Linux's gfx_v11_0_ring_soft_recovery), and every wave if it still stands half as long
// again; 0 turns it off. One hung app (Geekbench's OpenCL Feature Matching, 2026-10-26) otherwise froze WindowServer
// until its watchdog took the session down; a spinning wave killed this way by hand freed the queue in place.
static int gHangMs = 3000;
static volatile SInt32 gHangKills;
SYSCTL_INT(_debug, OID_AUTO, phx_hang_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gHangMs, 0,
           "ms without progress before a hung submission's waves are killed (0 off)");
SYSCTL_INT(_debug, OID_AUTO, phx_hang_kills, CTLFLAG_RD | CTLFLAG_LOCKED, (int *)&gHangKills, 0,
           "hung submissions whose waves were killed");
// debug.phx_vcn_wait: how the decode path waits for the hardware fence. The old loop slept in whole
// IOSleep(1) ticks even when the fence had already moved; the decoder retires inside a fraction of a
// millisecond on a P frame, so most of a 1 ms tick was wasted (2026-11-23). 0 keeps the bring-up loop.
static int gVcnWait = 2, gVcnSpinMaxUs = 200, gVcnSliceUs = 50;
// debug.phx_vcn_hw_ewma_ns: the P-frame hardware time the fence wait adapts to.
// Read it to see what the decoder is doing; write it to reset the adaptation.
static volatile SInt64 gVcnHwEwmaNs = 1000000;
SYSCTL_INT(_debug, OID_AUTO, phx_vcn_wait, CTLFLAG_RW | CTLFLAG_LOCKED, &gVcnWait, 0,
           "decode fence wait: 0 IOSleep(1), 1 sub-millisecond sleep, 2 spin then sleep");
SYSCTL_INT(_debug, OID_AUTO, phx_vcn_spin_max_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gVcnSpinMaxUs, 0,
           "longest spin before sleeping, microseconds (0 disables the spin)");
SYSCTL_INT(_debug, OID_AUTO, phx_vcn_slice_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gVcnSliceUs, 0,
           "sub-millisecond sleep slice, microseconds");
SYSCTL_QUAD(_debug, OID_AUTO, phx_vcn_hw_ewma_ns, CTLFLAG_RW | CTLFLAG_LOCKED, (SInt64 *)&gVcnHwEwmaNs,
            "P-frame hardware time moving average, nanoseconds");
// debug.phx_avc_idr_reuse: an IDR used to close and recreate the firmware session (three submissions,
// 12-16 ms) even though the decoder state it needs is carried in every frame message. Reuse it when the
// geometry, profile and reference count it was created for are unchanged; a failed frame forces a rebuild.
static int gAvcIdrReuse = 1;
SYSCTL_INT(_debug, OID_AUTO, phx_avc_idr_reuse, CTLFLAG_RW | CTLFLAG_LOCKED, &gAvcIdrReuse, 0,
           "reuse the firmware AVC session across IDRs: 0 recreates it every time");
// debug.phx_avc_flush_mode: how much of the decode target is invalidated before the readback copy.
// 0 flushes the whole target, 1 only the visible rows (default), 2 flushes each row as it is copied.
static int gAvcFlushMode = 1;
SYSCTL_INT(_debug, OID_AUTO, phx_avc_flush_mode, CTLFLAG_RW | CTLFLAG_LOCKED, &gAvcFlushMode, 0,
           "readback flush range: 0 whole target, 1 visible rows, 2 interleaved per row");
// GC 11.0.1 registers (segment 0 base 0x1260, segment 1 base 0xa000)
static constexpr uint32_t kRegSqCmd = 0x1260 + 0x111b, kRegRlcSafeMode = 0xa000 + 0x0980;
static volatile UInt32 gSysctlUp;

// psp_gfx_rb_frame (64 B) — the ring entry the PSP reads. Only 5 fields are set; the rest stay zero
// (Linux amdgpu_psp.c:3485: a non-zero cmd_buf_size makes the PSP silently drop the frame).
struct PhxGfxRbFrame {
    uint32_t cmd_buf_addr_lo, cmd_buf_addr_hi, cmd_buf_size;
    uint32_t fence_addr_lo, fence_addr_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PhxGfxRbFrame) == 64, "PhxGfxRbFrame must be 64 B");

// Canned "phoenix" table (Mesa amdgpu_devices.c): GB_ADDR_CONFIG and GC 11.0.1.
static constexpr uint32_t kGbAddrConfig = 0x00000242;
static constexpr uint32_t kGcVersion = (11u << 16) | (0u << 8) | 1u;

// Pools reported to RADV. E1 backs every BO with system memory, so these are budgets, not hardware:
// BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB.
static constexpr uint64_t kVisTotal = 256ull << 20;
// F7BSC (build 35): every pool is wired system RAM on this APU (32 GiB), so the 1536/512 MiB budgets only made
// WindowServer fail allocations (SkyLight then aborts -> back to the login window). Raised; RADV sizes its heaps from
// vis + GTT, so the plugin also sees a 1.5 GiB "card" instead of 512 MiB and stops evicting/spilling as early.
static constexpr uint64_t kHiTotal = 3840ull << 20;
static constexpr uint64_t kGttCap = 2048ull << 20;
static constexpr uint64_t kGttMaxBo = 1024ull << 20;
static constexpr uint64_t kHostCap = 4096ull << 20, kHostMaxBo = 1024ull << 20;   // host imports, per client
static constexpr uint64_t kVaMask = (1ull << 48) - 1;
static constexpr int kMaxMaps = 4096;

static bool vaCanonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

class PhxBringup : public IOService {
    OSDeclareDefaultStructors(PhxBringup)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2,
                                  void *p3, void *p4) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;

    bool claimSession();
    void releaseSession();
    IOReturn readRegs(uint32_t first, uint32_t count, uint32_t *out);
    IOReturn vramRead(uint32_t byteOff, uint32_t count, uint32_t *out);
    IOReturn writeReg(uint32_t reg, uint32_t value, uint32_t *readback);
    IOReturn vramWrite(uint32_t byteOff, uint32_t value);
    IOReturn doorbellWrite(uint64_t byteOff, uint64_t value);
    IOReturn sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1);
    IOReturn smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);
    IOReturn pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]);
    IOReturn pspSubmit(uint32_t cmdId, uint64_t out[4]);
    IOReturn fwStage(uint32_t dstOffset, const void *bytes, uint32_t len);
    IOReturn loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]);
    IOReturn pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]);
    IOReturn pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]);
    bool debugEnabled() const { return debug; }
    // E8: GPU execution. The userspace bring-up maps MES queues (compute = 0, gfx = 1) and hands them over (adoptQueue);
    // user BOs are then mapped into the VMID 8 page table and SUBMIT writes INDIRECT_BUFFERs into the queue's ring.
    IOReturn adoptQueue(const uint64_t in[6]);
    IOReturn claimAccel();
    bool gpuReady() const { return gq[0].ready || gq[1].ready; }
    // one GPUVM context (VMID 8..15) per client process: RADV instances allocate VAs independently
    uint32_t vmidAlloc();
    void vmidFree(uint32_t vmid, uint64_t lastSeq);
    IOReturn vmMap(uint32_t vmid, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size);
    IOReturn vmUnmap(uint32_t vmid, uint64_t va, uint64_t size);
    IOReturn gpuSubmit(uint32_t vmid, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seq);
    uint64_t gpuRetired() const;
    const phx_topology *capturedTopology() const { return topology; }
    IOReturn setPowerState(unsigned long ordinal, IOService *whichDevice) override;
    // VCN 4.0.2 bring-up (hard-coded for Phoenix). Reached only through debug selector 120, never at boot: the
    // PSP firmware load, the SMU power-up, the register sequence and the ring test all happen on demand, and a
    // failure powers the block down again. out[] layout is documented in PhxNativeABI.h (PHXN_SEL_DBG_VCN).
    IOReturn vcnStatus(uint64_t out[8]);
    IOReturn vcnInit(uint32_t fwOff, uint32_t fwLen, uint64_t tmrMc, uint32_t stopStage, uint64_t sramBytes,
                     uint64_t out[8]);
    IOReturn vcnRingTest(uint64_t out[8]);
    IOReturn vcnFenceTest(uint64_t out[8]);
    IOReturn vcnCodecSessionTest(bool encode, uint64_t out[8]);
    IOReturn vcnCodecFrame(bool encode, uint8_t *output, uint32_t capacity, uint64_t out[8],
                           const uint8_t *packedNv12 = nullptr, const EncodeGeometry &geometry = {},
                           const uint8_t *referenceIDR = nullptr);
    bool vcnEncoderReady() const;
    IOReturn vcnShutdown(uint64_t out[8]);
    IOReturn vcnLoadToc(uint32_t fwOff, uint32_t size, uint64_t out[8]);

private:
    friend class IOAccelPhxNativeClient;
    IOReturn encodeH264Descriptors(IOMemoryDescriptor *input, IOMemoryDescriptor *output,
                                   const EncodeGeometry &geometry, uint32_t *bytesWritten);
    IOReturn decodeReferenceIDR(const uint8_t *input, IOMemoryDescriptor *output, uint32_t *bytesWritten);
    IOReturn decodeAvcStream(uint64_t client, const uint8_t *au, size_t size, IOMemoryDescriptor *output,
                             uint32_t pitch = 0, uint32_t chromaOffset = 0);
    IOReturn decodeAvcStreamLocked(uint64_t client, const uint8_t *au, size_t size, IOMemoryDescriptor *output,
                                   uint32_t pitch, uint32_t chromaOffset);
    struct AvcSession {
        AvcStreamParser parser{};
        uint64_t ownerID{0};
        // Keep firmware-visible ranges reserved through timeout until confirmed shutdown.
        uint32_t persistentBytes{0}, stagingBytes{0}, bankBytes{0};
        // Where this session's context and DPB live in the pools; meaningful while persistentBytes/bankBytes != 0.
        uint32_t contextOff{0}, dpbOff{0};
        IOMemoryMap *targetMap{nullptr};
        uint32_t targetOffset{0}, targetBytes{0}, targetMatches{0};
        // What the firmware session was created for, so an IDR can reuse it (debug.phx_avc_idr_reuse).
        uint32_t codedWidth{0}, codedHeight{0};
        uint8_t profileIdc{0}, maxRefs{0};
        bool errorFree{false};
        uint64_t idrReused{0};
    };
    IOReturn submitAvcPicture(uint32_t slot, uint64_t client, const AvcStreamSequence &sequence, const AvcStreamPicture &picture,
                              const AvcStreamGeometry &geometry, const uint8_t *vcl, uint32_t vclBytes,
                              uint8_t *pixels, uint32_t destPitch, uint32_t destChroma);
    uint32_t findAvcSlot(uint64_t ownerID) const;
    bool avcVramInUse(uint32_t byteOffset) const;
    void publishAvcSessionCount();
    bool avcSessionCommand(uint32_t slot, bool close, uint32_t width, uint32_t height);
    bool allocAvcMemory(uint32_t slot, const AvcStreamGeometry &g);
    bool closeAvcStreamLocked(uint32_t slot, bool restarting = false);
    bool readAvcPicture(AvcSession &session, const AvcStreamSequence &sequence, const AvcStreamGeometry &geometry,
                        const uint8_t *barTarget, uint8_t *pixels, uint32_t destPitch, uint32_t destChroma);
    static void releaseAvcTargetMap(AvcSession &session);
    void releaseAvcStream(uint64_t client);
    uint32_t rreg(uint32_t reg) const;
    void wreg(uint32_t reg, uint32_t value) const;
    uint32_t smuPoll() const;
    bool pspWaitTos(uint32_t timeoutMs) const;
    IOReturn pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]);
    void hdpFlush() const;
    void readFbLayout();
    void clearRing();

    const phx_topology *topology{nullptr};
    bool isSleeping{false};
    IOPCIDevice *pci{nullptr};
    IOMemoryMap *mmio{nullptr};
    IOLock *smuLock{nullptr};
    // UMA carve-out ("VRAM"): GPU MC address range and the system physical address behind it.
    uint64_t fbMcBase{0}, fbSize{0}, fbPhys{0};
    // BAR0 framebuffer aperture: the CPU window into the first fbAperLen bytes of VRAM. It is an MMIO
    // region above system RAM (phys 0x8_2040_0000 here, 256 MiB), so CPU writes through it reach the
    // GPU's local memory and never the macOS heap. This is how the PSP ring is initialised.
    IOMemoryMap *fbAper{nullptr};
    IOMemoryMap *doorbellMap{nullptr}; // BAR2: doorbell aperture (2 MiB)
    IOBufferMemoryDescriptor *sysBuf{nullptr}; // debug: physically contiguous system memory for GPU VM experiments
    uint64_t fbAperPhys{0}, fbAperLen{0};
    // MMIO dword to write 0 to for an HDP flush (0 if the NBIO remap register was implausible).
    uint32_t hdpFlushDword{0};
    // PSP KM ring (debug selector 101): MC address once created, else 0. The GPCOM command + fence buffers
    // (selector 103) live just above the ring in VRAM; offsets are VRAM byte offsets, Mc the matching MC address.
    uint64_t ringMc{0}, ringOff{0};
    uint64_t cmdOff{0}, cmdMc{0}, fenceOff{0}, fenceMc{0};
    uint32_t fenceCounter{0};
    // Firmware bring-up regions (set when the ring is created): TMR the PSP copies firmware into, and fw_buf
    // where the CPU stages ucode for LOAD_IP_FW. Offsets are VRAM byte offsets, Mc the matching MC addresses.
    uint64_t tmrOff{0}, tmrMc{0}, tmrSize{0};
    uint64_t fwBufOff{0}, fwBufMc{0}, fwBufSize{0};
    bool tmrSetUp{false};
    // E8 state: the adopted queues (ring/wb/fence are VRAM byte offsets reached through BAR0) and the VMID 8
    // page-table pool (physically contiguous system memory; page 0 is the root). Sequence numbers are global across
    // the queues; each queue's fence holds the last sequence it finished, and fseq/fend keep its outstanding
    // submissions in order (sequence, ring position after it).
    struct GpuQueue {
        bool ready, gfx;
        uint32_t ringOff, ringDw, wbOff, fenceOff;
        uint64_t dbByte, wptr, doneEnd;
        uint64_t fseq[64], fend[64];
        uint32_t fvmid[64];                     // the hardware VMID each outstanding submission runs under
        volatile uint32_t head, tail;
        uint64_t lastDone, lastProgress;        // the hang watchdog's view: fence seen last, and when it last moved
        uint64_t killedSeq; bool killedAll;
    };
    GpuQueue gq[2]{};
    uint64_t gSeq{0};
    uint64_t queueFence(const GpuQueue &q) const;
    IOLock *gpuLock{nullptr};
    // One GPUVM context (page tables) per client, and the eight hardware VMIDs lent to contexts at submission time,
    // as amdgpu does: a fixed VMID per client capped Metal at eight processes, and with every app allowed Metal the
    // ninth got no device (2026-10-20). A context's handle is its index + 1.
    // A context's page tables, in physically contiguous 64 KiB chunks allocated as it maps (most contexts need one or
    // two): a single 4 MiB contiguous pool per context failed once memory was fragmented. Table i is page i % 16 of
    // chunk i / 16; table 0 is the root.
    static constexpr uint32_t kPtChunkBytes = 64u << 10, kPtChunkPages = kPtChunkBytes / 4096, kPtChunks = 256;   // 4096 tables = 8 GiB mapped per process (64 capped it at ~2 GiB)
    struct VmCtx {
        IOBufferMemoryDescriptor *chunk[kPtChunks];
        uint64_t chunkPhys[kPtChunks];
        uint64_t phys;                    // the root table
        uint32_t pages, used;
        bool busy;
        uint32_t hw;                      // the hardware VMID pointed at these tables, 0 = none
    };
    static constexpr uint32_t kMaxCtx = 96;
    VmCtx vm[kMaxCtx]{};
    struct HwVmid {
        uint32_t ctx;                     // handle of the context it points at, 0 = free
        uint64_t lastSeq, lastUse;        // its newest submission; when it was last bound or used
    };
    HwVmid hwv[8]{};
    uint64_t hwTick{0};
    uint32_t vmBind(uint32_t handle);
    VmCtx *vmCtx(uint32_t handle) { return handle && handle <= kMaxCtx && vm[handle - 1].busy ? &vm[handle - 1] : nullptr; }
    uint64_t *ptTable(const VmCtx &c, uint64_t phys) const;
    uint64_t ptTablePhys(VmCtx &c, uint32_t i);
    uint64_t *vmPte(VmCtx &c, uint64_t va, bool alloc);
    void vmFlushTlb(uint32_t vmid) const;
    void ringPut(GpuQueue &q, uint32_t idx, uint32_t value);
    thread_call_t hangCall{nullptr};
    volatile bool hangStop{false};
    static void hangTick(thread_call_param_t self, thread_call_param_t);
    void hangCheck();
    void killWaves(uint32_t vmid, bool all);
    bool debug{false};
    bool smuUsed{false};
    volatile SInt32 sessionOpen{0};

    // --- VCN (hard-coded Phoenix VCN 4.0.2) -------------------------------------------------------------------
    IOLock *vcnLock{nullptr};
    bool vcnStopped{false};  // guarded by vcnLock, which lives through the last retained video context
    bool vcnReady{false};
    bool vcnNeedsPowerDown{false};
    bool vcnDpgActive{false};     // Hardware PG_MODE stays set after the init options have been cleared.
    uint64_t vcnFwMc{0};          // TMR MC address the PSP placed the firmware at (LOAD_IP_FW response)
    uint32_t vcnRingWptr{0};
    uint32_t vcnDbIndex{kDbIndexLinux};  // DWORD index in BAR2
    bool vcnSram{false};          // --dpg-sram: stage the DPG sequence into VRAM and let the PSP apply it
    bool vcnSramFull{false};      // --sram-full: pad the block to a full 4 KB page
    uint32_t vcnSramBytes{0};     // --sram-pad N: pad the block to N bytes (0 = exact used size)
    uint32_t vcnSramPage{0};      // --sram-page N: put the block in page N of the VRAM BO (0 = default 0xB1)
    uint32_t vcnSramTrim{0};      // --sram-trim N: keep only the first N pairs (0 = all)
    uint32_t vcnSramAbs{0};       // --sram-abs N: put the block at absolute aperture page N (0 = use the BO)
    bool vcnPspIgnore{false};     // --psp-ignore: continue despite a nonzero LOAD_IP_FW status (is the status real?)
    uint32_t vcnFwType{0};        // --fw-type N: LOAD_IP_FW firmware type (0 = kPspFwTypeVcnRam)
    uint32_t vcnSramBuf[1024]{};  // (internal offset, value) pairs, as Linux builds for indirect SRAM
    uint32_t vcnSramN{0};
    bool vcnDpgPgMode{false};     // --dpg-pg: also enable POWER_STATUS dynamic gating (it gates the block dead here)
    bool vcnDpg{false};           // debug: start through the DPG indirect port instead of the static path
    bool vcnPlanesOn{false};      // PGFSM planes were switched on by us: gate them before PowerDownVcn
    uint32_t vcnStopStage{0};     // debug: stop init after this stage (0 = run all), see PHX_VCN_STOPPED      // next dword slot in the unified ring
    uint32_t vcnFenceSeq{0};      // last fence value submitted through vcnFenceTest
    bool vcnSubmitCodecIb(uint32_t ibDw, uint32_t phase, uint64_t out[8], bool pFrame = false);
    uint32_t vcnIb[512]{};        // fixed VCN frame IB scratch, protected by vcnLock
    AvcSession avcSessions[kAvcStreamSlotCount]{}; // Guarded by vcnLock.
    bool avcMediaEnabled{false};
    bool avcUsedUma{false};
    uint64_t avcUploadEnd{0}, avcFenceEnd{0}, avcReadEnd{0}; // Timing of a stream submission under vcnLock.
    uint64_t avcFlushNs{0}, avcCopyNs{0};                    // Last readback: cache invalidation and copy.
    uint32_t avcSerial{0};  // feedback number of the last attempted frame across all sessions
    // These message and scaling arrays never occupy the kernel stack.
    uint32_t avcMessage[kAvcStreamMessageDw]{};
    uint8_t avcScaling[kAvcStreamScalingBytes]{};
    uint32_t vcnPowerUp(uint32_t ucodeSize, uint64_t out[8]);   // SMU power-up + registers (vcnLock held)
    bool vcnPowerDown(uint32_t *response);
    bool vcnStopDpg();
    IOReturn vcnSetDoorbell(uint32_t index);
    bool vcnConfigureDoorbell(uint64_t out[8]);
    uint32_t vcnStaticStart(uint32_t ucodeSize);
    uint32_t vcnSetupRing();
    uint32_t vcnSubmitFence(uint32_t *before, uint32_t *after, uint32_t *observed);
    // Breadcrumbs (debug): every VCN register access and SMU message is logged into VRAM at kCrumbOff *before* it
    // happens and read back through BAR0, so after a hard reset `vcnctl crumbs` shows the last access attempted.
    static constexpr uint32_t kCrumbOff = 0xC900000u, kCrumbN = 256u, kCrumbMagic = 0x4E435642u;   // "BVCN"
    void vcnCrumb(uint32_t tag, uint32_t value) const;
    void vcnCrumbValue(uint32_t value) const;
    void vcnCrumbReset() const;
    void vcnSweepDump() const;
    void vcnDisableClockGating();
    void dpgWrite(uint32_t reg, uint32_t value);
    uint32_t vcnDpgStart(uint32_t ucodeSize, uint64_t out[8]);
    void vcnVcpuDiag();
    uint32_t vcnCrumbMode{0};     // debug, per init call: 0 crumbs + read-back, 1 crumbs without read-back, 2 off
    uint32_t vrd(uint32_t reg) const { vcnCrumb(0x10000000u | reg, 0xEEEEEEEEu); const uint32_t v = rreg(reg); vcnCrumbValue(v); return v; }
    void vwr(uint32_t reg, uint32_t v) const { vcnCrumb(0x20000000u | reg, v); wreg(reg, v); }
    IOReturn vcnSmu(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *after) {
        vcnCrumb(0x30000000u | msg, arg);
        const IOReturn r = smuMessage(msg, arg, resp, after);
        vcnCrumb(0x40000000u | msg, *resp);
        return r;
    }
    uint32_t vcnLoadFirmware(uint32_t fwOff, uint32_t fwLen, uint64_t preloadMc, uint64_t out[8]);
    // Fence wait for the decode path (debug.phx_vcn_wait). The lock exists so the wait can sleep with a
    // deadline; a future IH interrupt handler wakes the same event without changing the waiting side.
    IOLock *vcnWaitLock{nullptr};
    uint32_t vcnWaitEvt{0};          // the average itself lives in gVcnHwEwmaNs (sysctl-visible)
    struct VcnWaitStats {
        uint64_t spinHits, shortHits, fallbackHits, sleeps, maxNs;
    } vcnWaitStats{};
    uint32_t vcnLastWaitSleeps{0}, vcnLastWaitSpinUs{0}, vcnLastWaitHit{0}, vcnLastWaitMode{0};
    uint32_t vcnLastIdrReuse{0};
    uint32_t vcnWaitFence(uint32_t wptr, uint32_t seq, uint32_t timeoutMs, uint32_t *after, uint32_t *observed,
                          bool updateEwma);
    uint32_t vcnSubmitTest(const uint32_t *packet, uint32_t count, uint32_t seq,
                           uint32_t *before, uint32_t *after, uint32_t *observed, uint32_t timeoutMs = 1000u,
                           bool hot = false, bool pFrame = false);
    void vcnRecord(uint64_t out[8]) const;
    bool vcnRegOk(uint32_t reg) const;
    bool vcnFrameMemoryOk(uint32_t scratchBytes, bool checkDpbBanks = false) const;
    bool vcnWaitReg(uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeoutMs) const;
    uint64_t vcnMc(uint32_t off) const { return fbMcBase + off; }
    uint32_t vcnReadVram(uint32_t off) const;
    void vcnWriteVram(uint32_t off, uint32_t value);
    void vcnZeroVram(uint32_t off, uint32_t bytes);
    void vcnSharedQueueMode(uint32_t set, uint32_t clear);
};

OSDefineMetaClassAndStructors(PhxBringup, IOService)

class IOAccelPhxNativeClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelPhxNativeClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    struct Bo {
        IOMemoryDescriptor *md;   // an IOBufferMemoryDescriptor, or the caller's own pages (host, prepared)
        uint64_t size;
        uint32_t domain;
        uint32_t bits;
        uint8_t *kva;             // the kernel's view of the BO; NULL for host imports, which the kernel never reads
        bool host;
    };
    struct Map {
        bool used;
        uint32_t handle;
        uint64_t va, size, off;
        uint32_t flags;
    };

    IOReturn doHello(IOExternalMethodArguments *a);
    IOReturn doQueryInfo(IOExternalMethodArguments *a);
    IOReturn doTopology(IOExternalMethodArguments *a);
    IOReturn doReadRegs(IOExternalMethodArguments *a);
    IOReturn doBoCreate(IOExternalMethodArguments *a);
    IOReturn doBoFree(IOExternalMethodArguments *a);
    IOReturn doBoImportHost(IOExternalMethodArguments *a);
    void boRelease(Bo &b);
    IOReturn doGemVa(IOExternalMethodArguments *a);
    IOReturn doCtx(IOExternalMethodArguments *a);
    IOReturn doSubmit(IOExternalMethodArguments *a);
    IOReturn doWaitSeq(IOExternalMethodArguments *a);
    IOReturn doSurfGpuMark(IOExternalMethodArguments *a);
    IOReturn doDbgSmu(IOExternalMethodArguments *a);
    IOReturn doDbgPspRing(IOExternalMethodArguments *a);
    IOReturn doDbgVramRead(IOExternalMethodArguments *a);
    IOReturn doDbgPspSubmit(IOExternalMethodArguments *a);
    IOReturn doDbgFwStage(IOExternalMethodArguments *a);
    IOReturn doDbgLoadIpFw(IOExternalMethodArguments *a);
    IOReturn doDbgPspCtl(IOExternalMethodArguments *a);
    IOReturn doDbgPspRaw(IOExternalMethodArguments *a);
    IOReturn doDbgWriteReg(IOExternalMethodArguments *a);
    IOReturn doDbgVramWrite(IOExternalMethodArguments *a);
    IOReturn doDbgDoorbell(IOExternalMethodArguments *a);
    IOReturn doDbgSysMem(IOExternalMethodArguments *a);
    IOReturn doDbgAdoptQueue(IOExternalMethodArguments *a);
    IOReturn doDbgClaim(IOExternalMethodArguments *a);
    IOReturn doDbgVcn(IOExternalMethodArguments *a);
    IOReturn doDbgAvcDecode(IOExternalMethodArguments *a);
    void releaseAvcProvider();
    IOReturn doEncodeH264(IOExternalMethodArguments *a, bool sized);
    void flushBos();
    Bo *findBo(uint32_t h);
    bool ibCovered(uint64_t vaCanon, uint64_t bytes);
    void freeAll();

    PhxBringup *owner{nullptr};
    PhxBringup *avcProvider{nullptr};  // retained for cleanup even after detach
    bool avcClientClosed{false};       // protected by lock
    IOLock *lock{nullptr};
    bool privileged{false};
    bool hello{false};
    bool sessionClaimed{false};
    uint32_t vmid{0};        // this client's GPUVM context, taken on its first GPU mapping or submission
    uint64_t lastSeq{0};     // its newest submission: the context is released only once the GPU is past it
    bool ensureVmid() { return vmid || (vmid = owner->vmidAlloc()) != 0; }
    Bo *bos{nullptr};      // PHXN_MAX_BOS entries, index = handle
    Map *maps{nullptr};    // kMaxMaps entries
    bool ctx[PHXN_MAX_CTX + 1]{};
    uint64_t visUsed{0}, hiUsed{0}, gttUsed{0}, hostUsed{0};
    task_t task{nullptr};
    uint64_t emitted{0};
    uint32_t pendFenceHandle{0}, pendFenceOffset{0};
    uint64_t pendFenceSeq{0};
};

OSDefineMetaClassAndStructors(IOAccelPhxNativeClient, IOUserClient)

// ---- service ---------------------------------------------------------------------------------------------

bool PhxBringup::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-phxbringupoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;
    vcnLock = IOLockAlloc();
    if (!vcnLock)
        return false;
    vcnWaitLock = IOLockAlloc();
    if (!vcnWaitLock)
        return false;
    pci->retain();
    {
        uint16_t v = pci->configRead16(0);
        uint16_t d = pci->configRead16(2);
        uint8_t rev = pci->configRead8(8);
        uint16_t sv = pci->configRead16(0x2c);
        uint16_t sd = pci->configRead16(0x2e);
        topology = phx_find_topology(v, d, rev, sv, sd);
        if (topology) {
            setProperty("phx-topology-cu", topology->cu_active_number, 32);
            setProperty("phx-topology-rb", topology->num_rb_pipes, 32);
            IOLog("PhxBringup: topology matched: %u active CUs, %u RBs for device 0x%04x:0x%04x\n",
                  topology->cu_active_number, topology->num_rb_pipes, v, d);
        }
    }
    PMinit();
    provider->joinPMtree(this);
    static IOPMPowerState pmStates[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
    };
    registerPowerDriver(this, pmStates, 2);
    {   // the GPU must be a bus master before it can DMA to/from system memory (page tables, user BOs)
        uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
        setProperty("phx-pci-cmd-before", cmd, 16);
        pci->setBusMasterEnable(true);
        pci->setMemoryEnable(true);
        setProperty("phx-pci-cmd-after", pci->configRead16(kIOPCIConfigCommand), 16);
    }
    mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    fbAper = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fbAper) {
        fbAperLen = fbAper->getLength();
        fbAperPhys = fbAper->getPhysicalAddress();
    }
    doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    smuLock = IOLockAlloc();
    gpuLock = IOLockAlloc();
    uint32_t dbg = 0;
    debug = PE_parse_boot_argn("phxdebug", &dbg, sizeof(dbg)) && dbg == 1;
    // The sysctls appear when the first client connects, too late to steer an early decode; the same names
    // as boot arguments cover that, and the sysctl overrides them afterwards.
    int bootValue = 0;
    if (PE_parse_boot_argn("phx_vcn_wait", &bootValue, sizeof(bootValue)) && bootValue >= 0 && bootValue <= 2)
        gVcnWait = bootValue;
    if (PE_parse_boot_argn("phx_vcn_spin_max_us", &bootValue, sizeof(bootValue)) && bootValue >= 0 && bootValue <= 2000)
        gVcnSpinMaxUs = bootValue;
    if (PE_parse_boot_argn("phx_vcn_slice_us", &bootValue, sizeof(bootValue)) && bootValue >= 10 && bootValue <= 1000)
        gVcnSliceUs = bootValue;
    if (PE_parse_boot_argn("phx_avc_idr_reuse", &bootValue, sizeof(bootValue)) && (bootValue == 0 || bootValue == 1))
        gAvcIdrReuse = bootValue;
    if (PE_parse_boot_argn("phx_avc_flush_mode", &bootValue, sizeof(bootValue)) && bootValue >= 0 && bootValue <= 2)
        gAvcFlushMode = bootValue;
    if (mmio) {
        readFbLayout();
        // HDP flush window: the NBIO remap register holds the BAR5 byte offset where writing 0 flushes the
        // host data path. Accept it only if it is a plausible dword offset inside our MMIO aperture.
        uint32_t remap = rreg(PHX_NBIO_REMAP_HDP_MEM_FLUSH_CNTL);
        if (remap && remap != 0xFFFFFFFFu && (remap & 3) == 0 && remap + 4 <= mmio->getLength())
            hdpFlushDword = remap >> 2;
        setProperty("phx-hdp-flush-dword", hdpFlushDword, 32);
    }
    setProperty("phx-build", PHX_KEXT_BUILD, 32);
    publishAvcSessionCount();
    if (vcnLock && gpuLock && smuLock && fbAper && mmio && topology) {
        setProperty(PHX_VIDEO_AVC_PROPERTY, static_cast<unsigned long long>(PHX_VIDEO_AVC_VERSION), 32);
        setProperty(PHX_VIDEO_DECODE_PROPERTY, static_cast<unsigned long long>(PHX_VIDEO_DECODE_VERSION), 32);
        if (pci->configRead16(kIOPCIConfigVendorID) == 0x1002 && pci->configRead16(kIOPCIConfigDeviceID) == 0x15bf)
            avcMediaEnabled = setProperty(PHX_VIDEO_DECODE_STREAM_PROPERTY,
                                          static_cast<unsigned long long>(PHX_VIDEO_DECODE_STREAM_VERSION), 32);
    }
    setProperty("phx-stage", "E3");
    setProperty("phx-debug", debug);
    setProperty("phx-fb-aper-phys", fbAperPhys, 64);
    setProperty("phx-fb-aper-len", fbAperLen, 64);
    IOLog("PhxBringup: service up (mmio %s, bar0 %s %llu MiB, debug %d)\n", mmio ? "mapped" : "unmapped",
          fbAper ? "mapped" : "unmapped", (unsigned long long)(fbAperLen >> 20), debug);
    hangCall = thread_call_allocate(hangTick, this);
    if (hangCall) {
        uint64_t d;
        clock_interval_to_deadline(500, kMillisecondScale, &d);
        thread_call_enter_delayed(hangCall, d);
    }
    registerService();
    return true;
}

void PhxBringup::stop(IOService *provider) {
    if (hangCall) {
        hangStop = true;   // a tick running now does not arm the next one
        thread_call_cancel_wait(hangCall);
        thread_call_free(hangCall);
        hangCall = nullptr;
    }
    if (gSysctlUp) {
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush);
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_unregister_oid(&sysctl__debug_phx_hang_ms);
        sysctl_unregister_oid(&sysctl__debug_phx_hang_kills);
        sysctl_unregister_oid(&sysctl__debug_phx_vcn_wait);
        sysctl_unregister_oid(&sysctl__debug_phx_vcn_spin_max_us);
        sysctl_unregister_oid(&sysctl__debug_phx_vcn_slice_us);
        sysctl_unregister_oid(&sysctl__debug_phx_vcn_hw_ewma_ns);
        sysctl_unregister_oid(&sysctl__debug_phx_avc_idr_reuse);
        sysctl_unregister_oid(&sysctl__debug_phx_avc_flush_mode);
        gSysctlUp = 0;
    }
    if (vcnLock) {
        IOLockLock(vcnLock);
        vcnStopped = true;
        uint32_t response = 0;
        if (vcnNeedsPowerDown && !isSleeping)
            vcnPowerDown(&response);
        for (auto &session : avcSessions) releaseAvcTargetMap(session);
        isSleeping = true;
        IOLockUnlock(vcnLock);
    }
    clearRing();
    if (fbAper) {
        fbAper->release();
        fbAper = nullptr;
    }
    if (sysBuf) {
        sysBuf->release();
        sysBuf = nullptr;
    }
    gq[0].ready = gq[1].ready = false;
    for (VmCtx &c : vm) {
        for (IOBufferMemoryDescriptor *b : c.chunk)
            if (b)
                b->release();
        c = VmCtx{};
    }
    for (HwVmid &h : hwv)
        h = HwVmid{};
    if (gpuLock) {
        IOLockFree(gpuLock);
        gpuLock = nullptr;
    }
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
    }
    if (mmio) {
        mmio->release();
        mmio = nullptr;
    }
    if (pci) {
        pci->release();
        pci = nullptr;
    }
    if (smuLock) {
        IOLockFree(smuLock);
        smuLock = nullptr;
    }
    PMstop();
    IOService::stop(provider);
}

void PhxBringup::free() {
    if (vcnWaitLock) IOLockFree(vcnWaitLock);
    if (vcnLock) IOLockFree(vcnLock);
    IOService::free();
}

IOReturn PhxBringup::setPowerState(unsigned long ordinal, IOService *whichDevice) {
    IOLockLock(vcnLock);
    if (vcnStopped) {
        IOLockUnlock(vcnLock);
        return IOPMAckImplied;
    }
    if (ordinal == 0) {
        IOLog("PhxBringup: powerStateWillChange to sleep (state 0)\n");
        uint32_t response = 0;
        if (vcnNeedsPowerDown && !isSleeping)
            vcnPowerDown(&response);
        for (auto &session : avcSessions) releaseAvcTargetMap(session);
        vcnFwMc = 0;
        isSleeping = true;
    } else {
        IOLog("PhxBringup: powerStateDidChange from sleep (state 1)\n");
        isSleeping = false;
    }
    IOLockUnlock(vcnLock);
    return IOPMAckImplied;
}

uint32_t PhxBringup::rreg(uint32_t reg) const {
    return reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg];
}

void PhxBringup::wreg(uint32_t reg, uint32_t value) const {
    reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg] = value;
}

// FB location (MC, 16 MiB units, FB_BASE bits 23:0) and the system physical address of the carve-out (FB_OFFSET << 24).
void PhxBringup::readFbLayout() {
    uint32_t base = rreg(PHX_MMMC_VM_FB_LOCATION_BASE) & 0x00FFFFFF;
    uint32_t top = rreg(PHX_MMMC_VM_FB_LOCATION_TOP) & 0x00FFFFFF;
    fbMcBase = static_cast<uint64_t>(base) << 24;
    fbSize = top >= base ? (static_cast<uint64_t>(top - base) + 1) << 24 : 0;
    fbPhys = static_cast<uint64_t>(rreg(PHX_MMMC_VM_FB_OFFSET)) << 24;
    setProperty("phx-fb-mc-base", fbMcBase, 64);
    setProperty("phx-fb-size", fbSize, 64);
    setProperty("phx-fb-phys", fbPhys, 64);
    // About This Mac / System Information name the GPU from the PCI device's "model" and size its memory from
    // "VRAM,totalMB"; without them the 780M read "Display, 15 MB" (the boot framebuffer)
    static const char kModel780[] = "AMD Radeon 780M";
    static const char kModel760[] = "AMD Radeon 760M";
    static const char kModel740[] = "AMD Radeon 740M";
    const char *model = kModel780;
    unsigned modelSize = sizeof(kModel780);
    if (topology) {
        if (topology->cu_active_number <= 4) {
            model = kModel740; modelSize = sizeof(kModel740);
        } else if (topology->cu_active_number <= 8) {
            model = kModel760; modelSize = sizeof(kModel760);
        }
    }
    if (OSData *m = OSData::withBytes(model, modelSize)) {
        pci->setProperty("model", m);
        m->release();
    }
    if (fbSize)
        pci->setProperty("VRAM,totalMB", fbSize >> 20, 32);
}

void PhxBringup::clearRing() {
    ringMc = ringOff = 0;
    cmdOff = cmdMc = fenceOff = fenceMc = 0;
    fenceCounter = 0;
    tmrOff = tmrMc = tmrSize = 0;
    fwBufOff = fwBufMc = fwBufSize = 0;
    tmrSetUp = false;
}

// Drain the host data path so the PSP sees our VRAM writes before we ring the doorbell (C2PMSG_67).
void PhxBringup::hdpFlush() const {
    if (hdpFlushDword)
        wreg(hdpFlushDword, 0);
    (void)rreg(0x0DE3);   // read-back barrier (GRBM_STATUS), as in the reference
}

// Read-only: count dwords from VRAM at byteOff, through the BAR0 aperture. Used by debug selector 102 to
// find the live console framebuffer and a free (all-zero) VRAM region for the PSP structures.
IOReturn PhxBringup::vramRead(uint32_t byteOff, uint32_t count, uint32_t *out) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || (static_cast<uint64_t>(byteOff) + static_cast<uint64_t>(count) * 4) > fbAperLen)
        return kIOReturnBadArgument;
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff);
    for (uint32_t i = 0; i < count; i++)
        out[i] = v[i];
    return kIOReturnSuccess;
}

// Write one MMIO dword (bring-up experimentation; root + phxdebug only). Returns the readback.
IOReturn PhxBringup::writeReg(uint32_t reg, uint32_t value, uint32_t *readback) {
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    wreg(reg, value);
    __asm__ __volatile__("mfence" ::: "memory");
    *readback = rreg(reg);
    return kIOReturnSuccess;
}

// Write one dword into VRAM at byteOff through the BAR0 aperture.
// op 1: allocate a physically contiguous buffer of a bytes (replaces any previous), out0 = physical address
// op 2: read dword at byte offset a -> out1;  op 3: write dword b at byte offset a;  op 4: free
IOReturn PhxBringup::sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1) {
    *out0 = *out1 = 0;
    if (op == 1) {
        if (sysBuf) {
            sysBuf->release();
            sysBuf = nullptr;
        }
        if (a == 0 || a > (64ull << 20))
            return kIOReturnBadArgument;
        sysBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, a, 0x00007FFFFFFFF000ull);
        if (!sysBuf)
            return kIOReturnNoMemory;
        bzero(sysBuf->getBytesNoCopy(), a);
        *out0 = sysBuf->getPhysicalAddress();
        *out1 = sysBuf->getLength();
        return kIOReturnSuccess;
    }
    if (!sysBuf)
        return kIOReturnNotReady;
    if (op == 4) {
        sysBuf->release();
        sysBuf = nullptr;
        return kIOReturnSuccess;
    }
    if ((a & 3) || a + 4 > sysBuf->getLength())
        return kIOReturnBadArgument;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(sysBuf->getBytesNoCopy()) + a);
    if (op == 2) {
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
        *out1 = *p;
        *out0 = sysBuf->getPhysicalAddress();
        return kIOReturnSuccess;
    }
    if (op == 3) {
        *p = static_cast<uint32_t>(b);
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); // GPU walker may not snoop CPU caches
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

// ---- E8: GPU execution ------------------------------------------------------------------------------------
// Everything here reproduces what tools/phxctl/mesboot.c proved on the 780M: VMID 8 with a 4-level table in
// system memory (PDE = next|VALID|SYSTEM|SNOOPED, no BFS/TF since gmc_v11 has translate_further off), system-page
// PTEs with MTYPE 4, invalidation on engine 17, and an ACQUIRE/RELEASE_MEM GL2 writeback after each IB.
static constexpr uint32_t kFirstUserVmid = 8, kNumUserVmids = 8;   // VMIDs 8..15, lent to client contexts (vmBind)
static constexpr uint64_t kPteSysFlags = 0x77ull | (4ull << 48);  // VALID SYSTEM SNOOPED EXEC READ WRITE, MTYPE 4
static constexpr uint64_t kPtAddrMask = 0x0000FFFFFFFFF000ull;
// GC registers (absolute dword index, GC base 0x1260): context n = context 0 + n (CNTL) / + 2n (address pairs),
// invalidation engine 17
static constexpr uint32_t kRegCtx0Cntl = 0x28E8, kRegCtx0BaseLo = 0x2953, kRegCtx0StartLo = 0x2973, kRegCtx0EndLo = 0x2993;
static constexpr uint32_t kRegInvEng17Req = 0x291C, kRegInvEng17Ack = 0x292E;

static inline void flushLine(const volatile void *p) {
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
}

uint64_t *PhxBringup::ptTable(const VmCtx &c, uint64_t phys) const {
    for (uint32_t i = 0; i < kPtChunks && c.chunk[i]; i++)
        if (phys - c.chunkPhys[i] < kPtChunkBytes)
            return reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(c.chunk[i]->getBytesNoCopy()) + (phys - c.chunkPhys[i]));
    return nullptr;
}

// The physical address of table i, allocating (zeroed, flushed) the chunk it lives in; 0 when that fails.
uint64_t PhxBringup::ptTablePhys(VmCtx &c, uint32_t i) {
    const uint32_t k = i / kPtChunkPages;
    if (k >= kPtChunks)
        return 0;
    if (!c.chunk[k]) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kPtChunkBytes, kPtAddrMask);
        if (!b)
            return 0;
        uint8_t *p = static_cast<uint8_t *>(b->getBytesNoCopy());
        bzero(p, kPtChunkBytes);
        for (uint32_t o = 0; o < kPtChunkBytes; o += 64)
            flushLine(p + o);
        c.chunk[k] = b;
        c.chunkPhys[k] = b->getPhysicalAddress();
    }
    return c.chunkPhys[k] + static_cast<uint64_t>(i % kPtChunkPages) * 4096;
}

uint64_t *PhxBringup::vmPte(VmCtx &c, uint64_t va, bool alloc) {
    uint64_t tbl = c.phys;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = ptTable(c, tbl);
        if (!t)
            return nullptr;
        uint64_t *e = t + ((va >> shift) & 511);
        if (!(*e & 1)) {
            if (!alloc || c.used >= c.pages)
                return nullptr;
            const uint64_t child = ptTablePhys(c, c.used);
            if (!child)
                return nullptr;
            c.used++;
            *e = child | 7;
            flushLine(e);
        }
        tbl = *e & kPtAddrMask;
    }
    uint64_t *t = ptTable(c, tbl);
    return t ? t + ((va >> 12) & 511) : nullptr;
}

void PhxBringup::vmFlushTlb(uint32_t vmid) const {
    __asm__ __volatile__("mfence" ::: "memory");
    wreg(kRegInvEng17Req, 0x00F80000u | (1u << vmid) | 1u);
    for (int i = 0; i < 1000 && !(rreg(kRegInvEng17Ack) & (1u << vmid)); i++)
        IODelay(1);
}

// Takes a free context: empty tables (root only), no hardware VMID until its first submission. Returns its handle,
// 0 when all kMaxCtx are in use.
uint32_t PhxBringup::vmidAlloc() {
    if (!gpuReady())
        return 0;
    IOLockLock(gpuLock);
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        VmCtx &c = vm[i];
        if (c.busy)
            continue;
        if (!c.chunk[0]) {
            c.phys = ptTablePhys(c, 0);
            if (!c.phys)
                break;
            c.pages = kPtChunks * kPtChunkPages;
        }
        c.used = 1;
        c.busy = true;
        c.hw = 0;
        IOLockUnlock(gpuLock);
        return i + 1;
    }
    IOLockUnlock(gpuLock);
    return 0;
}

// Points a hardware VMID at the context's tables for a submission (gpuLock held): the one it already has, a free one,
// or the least recently used one whose submissions the GPU has all finished (waits for one, bounded ~2 s). 0 on timeout.
uint32_t PhxBringup::vmBind(uint32_t handle) {
    VmCtx &c = vm[handle - 1];
    const uint64_t now = ++hwTick;
    if (c.hw) {
        hwv[c.hw - kFirstUserVmid].lastUse = now;
        return c.hw;
    }
    int pick = -1;
    for (int spin = 0; pick < 0; spin++) {
        const uint64_t done = gpuRetired();
        uint64_t oldest = ~0ull;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {
            if (!hwv[i].ctx) {
                pick = static_cast<int>(i);
                break;
            }
            if (hwv[i].lastSeq <= done && hwv[i].lastUse < oldest) {
                oldest = hwv[i].lastUse;
                pick = static_cast<int>(i);
            }
        }
        if (pick >= 0)
            break;
        if (spin > 40000)
            return 0;
        IODelay(50);
    }
    HwVmid &h = hwv[pick];
    if (h.ctx)
        vm[h.ctx - 1].hw = 0;
    h.ctx = handle;
    h.lastSeq = 0;
    h.lastUse = now;
    const uint32_t vmid = kFirstUserVmid + static_cast<uint32_t>(pick);
    c.hw = vmid;
    wreg(kRegCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 3));
    wreg(kRegCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
    wreg(kRegCtx0Cntl + vmid, 0x01fffe07);
    wreg(kRegCtx0StartLo + 2 * vmid, 0);
    wreg(kRegCtx0StartLo + 2 * vmid + 1, 0);
    wreg(kRegCtx0EndLo + 2 * vmid, 0xffffffff);
    wreg(kRegCtx0EndLo + 2 * vmid + 1, 0xf);
    vmFlushTlb(vmid);
    return vmid;
}

// Returns a context once the GPU is past the client's last submission (bounded wait): the tables used so far are
// cleared, so nothing the client mapped stays reachable, and its hardware VMID (if any) is freed.
void PhxBringup::vmidFree(uint32_t handle, uint64_t lastSeq) {
    if (!vmCtx(handle))
        return;
    for (int i = 0; i < 2000 && gpuRetired() < lastSeq; i++)
        IOSleep(1);
    IOLockLock(gpuLock);
    VmCtx &c = vm[handle - 1];
    for (uint32_t k = 0; k < kPtChunks && c.chunk[k] && k * kPtChunkPages < c.used; k++) {
        const uint32_t n = c.used - k * kPtChunkPages < kPtChunkPages ? c.used - k * kPtChunkPages : kPtChunkPages;
        uint8_t *p = static_cast<uint8_t *>(c.chunk[k]->getBytesNoCopy());
        bzero(p, static_cast<size_t>(n) * 4096);
        for (uint32_t o = 0; o < n * 4096u; o += 64)
            flushLine(p + o);
    }
    if (c.hw) {
        vmFlushTlb(c.hw);
        hwv[c.hw - kFirstUserVmid].ctx = 0;
        c.hw = 0;
    }
    c.used = 1;
    c.busy = false;
    IOLockUnlock(gpuLock);
}

static bool rangeFits(uint64_t start, uint64_t bytes, uint64_t limit) {
    return start <= limit && bytes <= limit - start;
}

// Queue/TMR adoption reserves the largest shared frame scratch. Submissions
// check the range actually used. The encoder and decoder share vcnLock.
constexpr uint32_t kVcnReservedScratch = kEncodeScratchMax + kAvcStreamScratchLimit;
static_assert(kEncodeScratchMax >= kDecodeFrameRequiredBytes &&
              kDecodeFrameScratchOffset + kEncodeScratchMax == kAvcStreamScratchOffset &&
              kDecodeFrameScratchOffset + kVcnReservedScratch <= kVisTotal, "VCN scratch exceeds BAR0");
// Codec contexts live in the aperture so the IDR path can zero them through BAR0;
// the DPB banks are GPU-only and sit above it.
static_assert(PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE <= kAvcContextBankOffset &&
              kAvcContextBankOffset + kAvcContextPoolBytes <= kBoOffset, "AVC context pool placement");
static bool vcnFrameOverlap(uint64_t offset, uint64_t bytes, uint32_t scratchBytes,
                            uint64_t fbBase = 0, bool checkDpbBanks = false) {
    const auto intersects = [offset, bytes](uint64_t base, uint64_t length) {
        return bytes && (offset < base ? base - offset < bytes : offset - base < length);
    };
    return intersects(fbBase + kBoOffset, kBoSize) ||
           (scratchBytes && intersects(fbBase + kDecodeFrameScratchOffset, scratchBytes)) ||
           intersects(fbBase + kAvcContextBankOffset, kAvcContextPoolBytes) ||
           (checkDpbBanks && intersects(fbBase + kAvcDpbBankOffset, kAvcDpbPoolBytes));
}

IOReturn PhxBringup::adoptQueue(const uint64_t in[6]) {
    if (!fbAper || !doorbellMap || !mmio || !fbMcBase || !gpuLock)
        return kIOReturnNotReady;
    // in[5] = fence offset | queue index (0 compute, 1 gfx): the fence is 8-byte aligned, so bit 0 is free
    const uint64_t ringOff = in[0], ringDw = in[1], wbOff = in[2], dbByte = in[3], wptr = in[4], fenceOff = in[5] & ~1ull;
    const uint32_t qi = static_cast<uint32_t>(in[5] & 1);
    if (ringDw < 256 || ringDw > 65536 || (ringDw & (ringDw - 1)) || (ringOff & 0xFF) ||
        !rangeFits(ringOff, ringDw * 4, fbAperLen) || (wbOff & 7) || !rangeFits(wbOff, 32, fbAperLen) ||
        (fenceOff & 7) || !rangeFits(fenceOff, 8, fbAperLen) || (dbByte & 7) ||
        !rangeFits(dbByte, 8, doorbellMap->getLength()) || ringOff > UINT32_MAX || wbOff > UINT32_MAX ||
        fenceOff > UINT32_MAX || vcnFrameOverlap(ringOff, ringDw * 4, kVcnReservedScratch, 0, true) ||
        vcnFrameOverlap(wbOff, 32, kVcnReservedScratch, 0, true) ||
        vcnFrameOverlap(fenceOff, 8, kVcnReservedScratch, 0, true))
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    GpuQueue &q = gq[qi];
    bzero(&q, sizeof(q));
    q.gfx = qi == 1;
    q.ringOff = static_cast<uint32_t>(ringOff);
    q.ringDw = static_cast<uint32_t>(ringDw);
    q.wbOff = static_cast<uint32_t>(wbOff);
    q.fenceOff = static_cast<uint32_t>(fenceOff);
    q.dbByte = dbByte;
    q.wptr = q.doneEnd = wptr;
    volatile uint32_t *fence = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
    fence[0] = static_cast<uint32_t>(gSeq);
    fence[1] = static_cast<uint32_t>(gSeq >> 32);
    hdpFlush();
    q.ready = true;
    IOLockUnlock(gpuLock);
    IOLog("PhxBringup: adopted %s queue ring=+0x%llx (%llu dw) wptr=%llu doorbell=+0x%llx\n",
          qi ? "gfx" : "compute", ringOff, ringDw, wptr, dbByte);
    return kIOReturnSuccess;
}

// PhxAccel (the NullMoth accelerator in its AMD configuration) starts only on a GPU marked as driven; matching is
// re-run so the accelerator personality, refused at boot, is offered the device again.
IOReturn PhxBringup::claimAccel() {
    if (!pci || !gpuReady())
        return kIOReturnNotReady;
    pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci->registerService();
    IOLog("PhxBringup: GPU claimed for PhxAccel, matching re-run\n");
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vmMap(uint32_t handle, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        IOByteCount segLen = 0;
        const addr64_t pa = md->getPhysicalSegment(off + p, &segLen, kIOMemoryMapperNone);
        uint64_t *pte = pa ? vmPte(c, va + p, true) : nullptr;
        if (!pte) {
            r = pa ? kIOReturnNoMemory : kIOReturnVMError;
            static uint32_t ptSaid;
            if (pa && (ptSaid++ < 8 || (ptSaid & 1023) == 0))
                IOLog("PhxBringup: GPU page tables of context %u full (%u of %u table pages) - a %llu-byte map refused\n",
                      handle, c.used, c.pages, (unsigned long long)size);
            break;
        }
        *pte = (pa & kPtAddrMask) | kPteSysFlags;
        flushLine(pte);
    }
    if (c.hw)   // an unbound context is flushed when it is next bound
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

IOReturn PhxBringup::vmUnmap(uint32_t handle, uint64_t va, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pte = vmPte(c, va + p, false);
        if (pte) {
            *pte = 0;
            flushLine(pte);
        }
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return kIOReturnSuccess;
}

void PhxBringup::ringPut(GpuQueue &q, uint32_t idx, uint32_t value) {
    const uint64_t slot = (q.wptr + idx) & (q.ringDw - 1);
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff + slot * 4) = value;
}

uint64_t PhxBringup::queueFence(const GpuQueue &q) const {
    const volatile uint32_t *f = reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + q.fenceOff);
    return static_cast<uint64_t>(f[0]) | (static_cast<uint64_t>(f[1]) << 32);
}

// Everything up to the returned sequence has finished: the oldest submission still running on any queue, minus one.
// Lock-free reader: an entry is written before `tail` moves past it.
uint64_t PhxBringup::gpuRetired() const {
    uint64_t r = gSeq;
    for (const GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++) {
            if (q.fseq[i & 63] > done) {
                if (q.fseq[i & 63] - 1 < r)
                    r = q.fseq[i & 63] - 1;
                break;
            }
        }
    }
    return r;
}

IOReturn PhxBringup::gpuSubmit(uint32_t handle, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    // graphics IBs go to the gfx queue and compute IBs to the compute queue; with one of them missing, the other runs both
    GpuQueue *qp = gfx ? (gq[1].ready ? &gq[1] : &gq[0]) : (gq[0].ready ? &gq[0] : &gq[1]);
    if (!qp->ready || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = *qp;
    const uint32_t need = n * 4 + 8;
    if (need + 16 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    // Ring space and the 64-entry history: retire finished entries, wait for the oldest one if needed (bounded, ~2 s).
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 16)
            break;
        if (spin > 40000) {
            IOLockUnlock(gpuLock);
            return kIOReturnBusy;
        }
        IODelay(50);
    }
    const uint32_t vmid = vmBind(handle);
    if (!vmid) {
        IOLockUnlock(gpuLock);
        return kIOReturnBusy;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ringPut(q, k++, 0xC0023F00);                               // INDIRECT_BUFFER
        // the CP wants the canonical (sign-extended) address: an upper-half IB (bit 47, where RADV puts them) with a
        // bare 16-bit high word silently wedges the queue
        const uint64_t cva = (ibVa[i] & (1ull << 47)) ? (ibVa[i] | 0xFFFF000000000000ull) : ibVa[i];
        ringPut(q, k++, static_cast<uint32_t>(cva) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(cva >> 32));
        // size | VMID, plus VALID on compute (gfx_v11_0_ring_emit_ib_compute; the gfx variant has no VALID bit)
        ringPut(q, k++, ibDw[i] | (q.gfx ? 0u : (1u << 23)) | (vmid << 24));
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0xC0064900);                                   // RELEASE_MEM, as gfx_v11_0_ring_emit_fence
    ringPut(q, k++, 0x06603514);                                   // GCR SEQ|GL2_WB|GLM_INV|GLM_WB, CP 3, CACHE_FLUSH_AND_INV_TS, idx 5
    ringPut(q, k++, 0x40000000);                                   // DATA_SEL 2 (64-bit), no interrupt
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, static_cast<uint32_t>(seq >> 32));
    ringPut(q, k++, 0);
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    q.fvmid[q.tail & 63] = vmid;
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    // BAR0 is write-combined: drain the WC buffers so the packets are in VRAM before the CP is told about them
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    volatile uint32_t *wb = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.wbOff + 16);
    wb[0] = static_cast<uint32_t>(q.wptr);
    wb[1] = static_cast<uint32_t>(q.wptr >> 32);
    __asm__ __volatile__("sfence" ::: "memory");
    (void)wb[0];
    hdpFlush();
    __asm__ __volatile__("mfence" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + q.dbByte) = q.wptr;
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

// Every 500 ms: a queue with unfinished work whose fence has not moved for debug.phx_hang_ms has the waves of its oldest
// unfinished submission killed, the CP then finishes that submission as if it had ended and the queue moves on.
void PhxBringup::hangTick(thread_call_param_t self, thread_call_param_t) {
    PhxBringup *me = static_cast<PhxBringup *>(self);
    me->hangCheck();
    if (me->hangStop)
        return;
    uint64_t d;
    clock_interval_to_deadline(500, kMillisecondScale, &d);
    thread_call_enter_delayed(me->hangCall, d);
}

void PhxBringup::hangCheck() {
    const int limitMs = gHangMs;
    if (limitMs <= 0 || !mmio)
        return;
    uint64_t limit, now = mach_absolute_time();
    nanoseconds_to_absolutetime(static_cast<uint64_t>(limitMs) * 1000000ull, &limit);
    for (GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        uint32_t oldest = 0;
        bool found = false;
        for (uint32_t i = q.head; i != tail && !found; i++)
            if (q.fseq[i & 63] > done) {
                oldest = i & 63;
                found = true;
            }
        if (!found || done != q.lastDone) {   // idle, or moving: the clock restarts
            q.lastDone = done;
            q.lastProgress = now;
            continue;
        }
        if (now - q.lastProgress < limit)
            continue;
        if (q.killedSeq != q.fseq[oldest]) {
            killWaves(q.fvmid[oldest], false);
            q.killedSeq = q.fseq[oldest];
            q.killedAll = false;
            q.lastProgress = now;
            IOLog("PhxBringup: %s queue stuck %d ms at seq %llu: killed the waves of VMID %u\n", q.gfx ? "gfx" : "compute",
                  limitMs, static_cast<unsigned long long>(q.fseq[oldest]), q.fvmid[oldest]);
        } else if (!q.killedAll && now - q.lastProgress >= limit / 2) {
            killWaves(0, true);
            q.killedAll = true;
            IOLog("PhxBringup: %s queue still stuck at seq %llu: killed every wave\n", q.gfx ? "gfx" : "compute",
                  static_cast<unsigned long long>(q.fseq[oldest]));
        }
    }
}

// gfx_v11_0_ring_soft_recovery: SQ_CMD kill (CMD 3) broadcast (MODE 1), limited to one VMID unless `all`, inside RLC
// safe mode
void PhxBringup::killWaves(uint32_t vmid, bool all) {
    wreg(kRegRlcSafeMode, 0x3);                        // CMD | MESSAGE 1: enter
    for (int i = 0; i < 1000 && (rreg(kRegRlcSafeMode) & 1); i++)
        IODelay(1);
    wreg(kRegSqCmd, 0x13u | (all ? 0u : (0x80u | (vmid & 0xfu) << 28)));
    wreg(kRegRlcSafeMode, 0x1);                        // CMD, MESSAGE 0: exit
    OSIncrementAtomic(&gHangKills);
}

IOReturn PhxBringup::doorbellWrite(uint64_t byteOff, uint64_t value) {
    if (!doorbellMap)
        return kIOReturnNotReady;
    if ((byteOff & 7) || byteOff + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + byteOff) = value;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vramWrite(uint32_t byteOff, uint32_t value) {
    if (!vcnLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    IOReturn status = kIOReturnSuccess;
    if (vcnStopped || !fbAper) status = kIOReturnNotReady;
    else if ((byteOff & 3) || !rangeFits(byteOff, 4, fbAperLen)) status = kIOReturnBadArgument;
    else if (avcVramInUse(byteOff)) status = kIOReturnBusy;
    else {
        *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff) = value;
        __asm__ __volatile__("sfence" ::: "memory");
    }
    IOLockUnlock(vcnLock);
    return status;
}

// psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK): response flag set and status 0.
bool PhxBringup::pspWaitTos(uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs * 10; i++) {
        if ((rreg(PHX_PSP_C2PMSG_64) & PHX_MBOX_TOS_MASK) == PHX_MBOX_TOS_READY)
            return true;
        IODelay(100);
    }
    return false;
}

// Debug selector 101: create / destroy the PSP 13.0.4 KM ring the way Linux psp_v13_0_ring_create / _stop and
// Cobalt64 psp_v14_0.cpp do. The ring lives in VRAM at carveOffset; the CPU writes it through the BAR0
// framebuffer aperture, which is MMIO above system RAM — so the PSP sees the bytes and the macOS heap is never
// touched. The MC address handed to the PSP is fbMcBase + carveOffset.
IOReturn PhxBringup::pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]) {
    if (!mmio || !fbAper || !fbMcBase)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    IOLockLock(smuLock);
    if (op == 1) {
        if (ringMc) {
            r = kIOReturnExclusiveAccess;   // a ring is already live; destroy it first
        } else if ((carveOffset & 0xFFF) || !rangeFits(carveOffset, PHX_PSP_FENCE_REL + 0x1000, PHX_VRAM_TMR_OFF) ||
                   PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE > fbAperLen) {
            r = kIOReturnBadArgument;        // ring+cmd+fence must sit below the TMR; fw_buf must fit the aperture
        } else {
            volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + carveOffset);
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                nonzero |= ring[i];
            out[2] = nonzero == 0;           // report whether the VRAM region was already clear (informational)
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                ring[i] = 0;                 // zero the ring buffer (Linux memsets ring_mem; safe via BAR0)
            if (!pspWaitTos(2000)) {
                r = kIOReturnNotReady;       // SOS not ready to accept the ring
            } else {
                ringMc = fbMcBase + carveOffset;
                wreg(PHX_PSP_C2PMSG_69, static_cast<uint32_t>(ringMc));
                wreg(PHX_PSP_C2PMSG_70, static_cast<uint32_t>(ringMc >> 32));
                wreg(PHX_PSP_C2PMSG_71, PHX_PSP_RING_SIZE);
                wreg(PHX_PSP_C2PMSG_64, PHX_PSP_RING_TYPE_KM << 16);
                IOSleep(20);
                if (!pspWaitTos(2000)) {
                    r = kIOReturnTimeout;    // raw C2PMSG_64 (with PSP status in bits 15:0) returned in out[0]
                    ringMc = 0;              // not established: allow destroy + retry
                } else {
                    // Ring is live. Lay the GPCOM command + fence buffers just above it and zero them,
                    // then reset the doorbell so wptr agrees with our slot 0 (Linux psp_v13_0_ring_create).
                    ringOff = carveOffset;
                    cmdOff = carveOffset + PHX_PSP_CMD_REL;
                    fenceOff = carveOffset + PHX_PSP_FENCE_REL;
                    cmdMc = fbMcBase + cmdOff;
                    fenceMc = fbMcBase + fenceOff;
                    fenceCounter = 0;
                    tmrOff = PHX_VRAM_TMR_OFF;
                    tmrMc = fbMcBase + tmrOff;
                    tmrSize = 0;
                    tmrSetUp = false;
                    fwBufOff = PHX_VRAM_FWBUF_OFF;
                    fwBufMc = fbMcBase + fwBufOff;
                    fwBufSize = PHX_VRAM_FWBUF_SIZE;
                    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
                    for (uint32_t i = 0; i < 0x1000 / 4; i++) {
                        cmd[i] = 0;
                        fen[i] = 0;
                    }
                    wreg(PHX_PSP_C2PMSG_67, 0);
                }
            }
        }
    } else if (op == 2) {
        wreg(PHX_PSP_C2PMSG_64, PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS);
        IOSleep(20);
        if (!pspWaitTos(2000))
            r = kIOReturnTimeout;
        clearRing();
    } else if (op != 3) {
        r = kIOReturnBadArgument;
    }
    out[0] = rreg(PHX_PSP_C2PMSG_64);
    out[1] = ringMc;
    out[3] = rreg(PHX_PSP_C2PMSG_67);
    IOLockUnlock(smuLock);
    return r;
}

// Core GPCOM submit (caller holds smuLock and a ring exists): write a prepared command — cmd_id plus up to
// nWords union dwords at +28 — into the VRAM command buffer, ring the doorbell, wait for the PSP's fence and
// read the response. Ports Linux psp_cmd_submit_buf / Cobalt64 psp_ring_cmd_submit.
IOReturn PhxBringup::pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]) {
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(base + cmdOff);
    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(base + fenceOff);

    // Command buffer: zero it, set cmd_id (buf_size/buf_version stay 0, as upstream does), then the union.
    for (uint32_t i = 0; i < PHX_PSP_CMD_BUF_SIZE / 4; i++)
        cmd[i] = 0;
    cmd[PHX_PSP_CMD_HDR_CMD_ID_OFFSET / 4] = cmdId;
    for (uint32_t i = 0; i < nWords && i < 14; i++)
        cmd[PHX_PSP_GFX_CMD_UNION_OFFSET / 4 + i] = unionWords[i];

    uint32_t fence = ++fenceCounter;
    fen[0] = 0;

    // Ring frame at the slot the current wptr points to.
    uint32_t wptr = rreg(PHX_PSP_C2PMSG_67);
    const uint32_t ringSizeDw = PHX_PSP_RING_SIZE / 4;         // 1024
    const uint32_t frameSizeDw = PHX_PSP_GFX_FRAME_SIZE / 4;   // 16
    uint32_t slot = (wptr % ringSizeDw == 0) ? 0 : wptr / frameSizeDw;
    if (slot >= PHX_PSP_RING_SIZE / PHX_PSP_GFX_FRAME_SIZE)
        return kIOReturnInternalError;
    PhxGfxRbFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(cmdMc);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(cmdMc >> 32);
    frame.fence_addr_lo = static_cast<uint32_t>(fenceMc);
    frame.fence_addr_hi = static_cast<uint32_t>(fenceMc >> 32);
    frame.fence_value = fence;
    volatile uint32_t *slotp =
        reinterpret_cast<volatile uint32_t *>(base + ringOff + static_cast<uint64_t>(slot) * PHX_PSP_GFX_FRAME_SIZE);
    const uint32_t *src = reinterpret_cast<const uint32_t *>(&frame);
    for (uint32_t i = 0; i < PHX_PSP_GFX_FRAME_SIZE / 4; i++)
        slotp[i] = src[i];

    // Drain CPU write buffers, flush the host data path, then ring the doorbell.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wptr = (wptr + frameSizeDw) % ringSizeDw;
    wreg(PHX_PSP_C2PMSG_67, wptr);

    // Wait for the PSP to write the fence value into VRAM (BAR0 read bypasses the CPU cache).
    uint32_t observed = 0, us = 0;
    const uint32_t budgetUs = 5000000;
    while (us < budgetUs) {
        observed = fen[0];
        if (observed == fence)
            break;
        IOSleep(1);
        us += 1000;
    }
    uint32_t status = cmd[PHX_PSP_GFX_RESP_OFFSET / 4];
    // Always succeed at the transport level so IOKit delivers the outputs; the PSP result is in out[].
    // out[1] = elapsed us (≈budget on a fence timeout), so userspace distinguishes timeout from a status code.
    out[0] = (observed == fence) ? status : 0xFFFFFFFFu;  // 0xFFFFFFFF = fence never arrived
    out[1] = us;
    out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 8) / 4];   // resp.fw_addr_lo / session_id
    out[3] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 12) / 4];  // resp.fw_addr_hi
    return kIOReturnSuccess;
}

// Selector 103: no-payload query command (FB_FW_RESERV_ADDR/EXT only) — validates the ring without touching GPU power.
IOReturn PhxBringup::pspSubmit(uint32_t cmdId, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR && cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR)
        return kIOReturnNotPermitted;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, nullptr, 0, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Selector 104: stage a chunk of firmware bytes into the VRAM fw_buf through BAR0 (no submit).
IOReturn PhxBringup::fwStage(uint32_t dstOffset, const void *bytes, uint32_t len) {
    if (!fbAper || !fwBufSize)
        return kIOReturnNotReady;
    if ((dstOffset & 3) || static_cast<uint64_t>(dstOffset) + len > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress() + fwBufOff + dstOffset);
    const uint8_t *s = static_cast<const uint8_t *>(bytes);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    if (i < len) {
        uint32_t v = 0;
        memcpy(&v, s + i, len - i);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    IOLockUnlock(smuLock);
    return kIOReturnSuccess;
}

// Selector 105: submit GFX_CMD_ID_LOAD_IP_FW for the firmware already staged in fw_buf at fwBufOffset.
IOReturn PhxBringup::loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if ((fwBufOffset & 3) || size == 0 || static_cast<uint64_t>(fwBufOffset) + size > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else {
        uint64_t mc = fwBufMc + fwBufOffset;
        uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, fwType};
        r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, out);
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 106: op 1 = SETUP_TMR (driver-managed TMR, Phoenix psp_13_0_4 is not in psp_skip_tmr), op 2 = AUTOLOAD_RLC.
IOReturn PhxBringup::pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else if (op == 1) {
        uint32_t size = arg ? arg : 0x400000u;
        if (size > PHX_VRAM_TMR_CAP)
            size = PHX_VRAM_TMR_CAP;
        // psp_gfx_cmd_setup_tmr (Linux psp_prep_tmr_cmd_buf): buf_phy_addr = TMR MC address, buf_size,
        // bitfield(virt_phy_addr=1 => 0x2), system_phy_addr = the REAL system-physical address of the TMR
        // (amdgpu_gmc_vram_pa = mc - vram_start + vram_base_offset = fbPhys + tmrOff on this UMA part).
        const uint64_t mc = fbMcBase + PHX_VRAM_TMR_OFF;
        const uint64_t tmrPa = fbPhys + PHX_VRAM_TMR_OFF;
        uint32_t u[6] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, 0x2u,
                         static_cast<uint32_t>(tmrPa), static_cast<uint32_t>(tmrPa >> 32)};
        r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
        if (r == kIOReturnSuccess && out[0] == 0) {
            tmrOff = PHX_VRAM_TMR_OFF;
            tmrMc = mc;
            tmrSize = size;
            tmrSetUp = true;
        }
    } else if (op == 2) {
        r = pspRunCmd(PHX_PSP_GFX_CMD_AUTOLOAD_RLC, nullptr, 0, out);
    } else {
        r = kIOReturnBadArgument;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 107: submit an arbitrary GPCOM command with six caller-supplied union dwords. For bring-up
// experiments (SETUP_TMR / LOAD_TOC parameter sweeps) without rebuilding the kext.
IOReturn PhxBringup::pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId == PHX_PSP_GFX_CMD_SETUP_TMR) {
        const uint64_t mc = (static_cast<uint64_t>(words[1]) << 32) | words[0];
        const uint64_t phys = (static_cast<uint64_t>(words[5]) << 32) | words[4];
        if (vcnFrameOverlap(mc, words[2], kVcnReservedScratch, fbMcBase, true) ||
            ((words[3] & 2u) && vcnFrameOverlap(phys, words[2], kVcnReservedScratch, fbPhys, true)))
            return kIOReturnBadArgument;
    }
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, words, 6, out) : kIOReturnNotReady;
    if (cmdId == PHX_PSP_GFX_CMD_SETUP_TMR && r == kIOReturnSuccess && out[0] == 0) {
        // bringup5.sh uses the raw PSP setup command. Record its successful MC window for VCN cache validation.
        tmrMc = (static_cast<uint64_t>(words[1]) << 32) | words[0];
        tmrSize = words[2];
        tmrSetUp = (words[3] & 0x2u) && tmrMc >= fbMcBase && tmrSize <= fbSize &&
                   tmrMc - fbMcBase <= fbSize - tmrSize;
        tmrOff = tmrSetUp ? tmrMc - fbMcBase : 0;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Polls the SMU response register for up to 2 s (Linux: usec_timeout * 20); 0 means no answer.
uint32_t PhxBringup::smuPoll() const {
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t r = rreg(PHX_SMU_RESP_REG);
        if (r)
            return r;
        IODelay(10);
    }
    return 0;
}

// One SMU 13.0.4 mailbox transaction, the sequence of Linux smu_msg_v1_send_msg().
IOReturn PhxBringup::smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    // The first message of the boot skips the pre-poll (Linux SMU_FW_INIT): nothing may have been sent yet.
    if (smuUsed && smuPoll() == 0) {
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    smuUsed = true;
    wreg(PHX_SMU_RESP_REG, 0);
    wreg(PHX_SMU_ARG_REG, arg);
    wreg(PHX_SMU_MSG_REG, msg);
    uint32_t r = smuPoll();
    *resp = r;
    *argAfter = rreg(PHX_SMU_ARG_REG);
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

// --- VCN 4.0.2 (hard-coded Phoenix) ---------------------------------------------------------------------------
//
// Linux's vcn_v4_0.c recipe, with every address and value fixed in PhxVcn.h: PSP LOAD_IP_FW stages the firmware
// in the TMR, SMU PowerUpVcn un-gates the block, the VCPU is pointed at the firmware (cache window 0), the stack
// and context windows and fw_shared, then the unified ring is enabled. The ring test writes VCN_ENC_CMD_END and
// waits for UVD_RB_RPTR to move, which is what amdgpu_vcn_enc_ring_test_ring does on this hardware.
//
// Selector 120 (root + phxdebug=1) starts the diagnostic engine. Sleep and driver teardown stop it if active.

volatile uint32_t *phxCrumbBase(IOMemoryMap *aper, uint64_t aperLen, uint32_t off, uint32_t n) {
    if (!aper || static_cast<uint64_t>(off) + 16u + n * 8u > aperLen)
        return nullptr;
    return reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(aper->getVirtualAddress()) + off);
}

// [0] magic [1] count [2] last tag [3] last value, then kCrumbN {tag, value} entries (ring). tag bits 31:28:
// 1 read (value 0xEEEEEEEE until the read returned), 2 write, 3 SMU message sent (low = msg, value = arg),
// 4 SMU answered (value = response), 5 stage reached (low = stage).
void PhxBringup::vcnCrumb(uint32_t tag, uint32_t value) const {
    if (vcnCrumbMode == 2)
        return;
    volatile uint32_t *c = phxCrumbBase(fbAper, fbAperLen, kCrumbOff, kCrumbN);
    if (!c || c[0] != kCrumbMagic)
        return;
    const uint32_t seq = c[1];
    volatile uint32_t *e = c + 4 + (seq % kCrumbN) * 2u;
    e[0] = tag;
    e[1] = value;
    c[2] = tag;
    c[3] = value;
    c[1] = seq + 1u;
    __asm__ __volatile__("sfence" ::: "memory");
    if (vcnCrumbMode == 0)
        (void)c[1];   // read back: the posted BAR writes reach DRAM before the access that follows
}

void PhxBringup::vcnCrumbValue(uint32_t value) const {
    if (vcnCrumbMode == 2)
        return;
    volatile uint32_t *c = phxCrumbBase(fbAper, fbAperLen, kCrumbOff, kCrumbN);
    if (!c || c[0] != kCrumbMagic || !c[1])
        return;
    c[4 + ((c[1] - 1u) % kCrumbN) * 2u + 1u] = value;
    c[3] = value;
    __asm__ __volatile__("sfence" ::: "memory");
    if (vcnCrumbMode == 0)
        (void)c[1];
}

// vcn_v4_0_disable_clock_gating: turn the block's own clock gating off before the VCPU is released. Without it the
// block can gate itself while the VCPU is starting, and UVD_STATUS never reports ready.
void PhxBringup::vcnDisableClockGating() {
    uint32_t d = vrd(kRegCgcCtrl);
    if (d != UINT32_MAX)
        vwr(kRegCgcCtrl, (d & ~kCgcCtrlDynMode) | kCgcCtrlTiming);
    d = vrd(kRegCgcGate);
    if (d != UINT32_MAX)
        vwr(kRegCgcGate, d & ~kCgcGateClear);
    if (!vcnWaitReg(kRegCgcGate, 0xFFFFFFFFu, 0u, 100u))
        IOLog("PhxBringup: VCN CGC_GATE 0x%08x did not clear\n", rreg(kRegCgcGate));
    d = vrd(kRegCgcCtrl);
    if (d != UINT32_MAX)
        vwr(kRegCgcCtrl, d & ~kCgcCtrlClear);
    d = vrd(kRegSuvdCgcGate);
    if (d != UINT32_MAX)
        vwr(kRegSuvdCgcGate, d | kSuvdCgcGateSet);
    d = vrd(kRegSuvdCgcCtrl);
    if (d != UINT32_MAX)
        vwr(kRegSuvdCgcCtrl, d & ~kSuvdCgcCtrlClear);
}

// Reads VCN's whole register window once (no breadcrumbs: 768 reads would flood the ring) and stores it in VRAM.
void PhxBringup::vcnSweepDump() const {
    if (!fbAper || static_cast<uint64_t>(kSweepOff) + 16u + kSweepCount * 4u > fbAperLen)
        return;
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(
        reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress()) + kSweepOff);
    d[0] = kSweepMagic;
    d[1] = kSweepFirst;
    d[2] = kSweepCount;
    d[3] = 0;
    for (uint32_t i = 0; i < kSweepCount; i++)
        d[4 + i] = vcnRegOk(kSweepFirst + i) ? rreg(kSweepFirst + i) : 0xDEADBEEFu;
    __asm__ __volatile__("sfence" ::: "memory");
    (void)d[0];
}

void PhxBringup::vcnCrumbReset() const {
    volatile uint32_t *c = phxCrumbBase(fbAper, fbAperLen, kCrumbOff, kCrumbN);
    if (!c)
        return;
    for (uint32_t i = 1; i < 4u + kCrumbN * 2u; i++)
        c[i] = 0;
    c[0] = kCrumbMagic;
    __asm__ __volatile__("sfence" ::: "memory");
    (void)c[0];
}

bool PhxBringup::vcnRegOk(uint32_t reg) const {
    return mmio && (static_cast<uint64_t>(reg) + 1u) * 4u <= mmio->getLength();
}

bool PhxBringup::vcnWaitReg(uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs; i++) {
        const uint32_t observed = vrd(reg);
        // 0xdeadbeef is the block's "read while gated" value: bit 1 is set in it, so it must never match a wait.
        if (observed != UINT32_MAX && observed != 0xDEADBEEFu && (observed & mask) == value)
            return true;
        IOSleep(1);
    }
    return false;
}

uint32_t PhxBringup::vcnReadVram(uint32_t off) const {
    if (!fbAper || (off & 3u) || static_cast<uint64_t>(off) + 4u > fbAperLen)
        return 0xFFFFFFFFu;
    return *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress()) + off);
}

void PhxBringup::vcnWriteVram(uint32_t off, uint32_t value) {
    if (!fbAper || (off & 3u) || static_cast<uint64_t>(off) + 4u > fbAperLen)
        return;
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress()) + off) = value;
}

void PhxBringup::vcnZeroVram(uint32_t off, uint32_t bytes) {
    if (!fbAper || !rangeFits(off, bytes, fbAperLen)) return;
    bzero(reinterpret_cast<uint8_t *>(fbAper->getVirtualAddress()) + off, bytes);
}

void PhxBringup::vcnSharedQueueMode(uint32_t set, uint32_t clear) {
    // An independent byte write avoids overwriting a concurrent firmware queue_status update.
    auto *mode = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress()) +
                 kBoOffset + kSharedOff + kSharedSq + 1u;
    *mode = static_cast<uint8_t>((*mode & ~clear) | set);
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
}

void PhxBringup::vcnRecord(uint64_t out[8]) const {
    if (isSleeping)
        return;
    // Only PGFSM is read unless it reports the M/S/LMI planes on; out[5]/[7] = 0xFFFFFFFF stand for "not read".
    const uint32_t pgfsm = vcnRegOk(kRegPgfsmStatus) ? vrd(kRegPgfsmStatus) : UINT32_MAX;
    out[6] = pgfsm;
    const bool on = vcnNeedsPowerDown && pgfsm != UINT32_MAX && pgfsm != 0xDEADBEEFu && (pgfsm & kPgfsmStatusMask) == kPgfsmPoweredPg;
    out[5] = on && vcnRegOk(kRegUvdStatus) ? vrd(kRegUvdStatus) : UINT32_MAX;
    out[7] = on && vcnRegOk(kRegRbRptr) ? vrd(kRegRbRptr) : UINT32_MAX;
}

bool PhxBringup::vcnStopDpg() {
    // vcnLock serializes commands with stop. Only a configured ring has a
    // usable fw_shared and write pointer. Partial init still needs PG_MODE cleared.
    bool drained = true;
    if (vcnReady) {
        vcnSharedQueueMode(kFwQueueDpgHoldOff, 0);
        drained = vcnWaitReg(kRegPowerStatus, kPowerStatusMask, kPowerStatusTilesOff, 400);
        if (drained) {
            const uint32_t target = vrd(kRegRbWptr);
            drained = target == vcnRingWptr && vcnWaitReg(kRegRbRptr, UINT32_MAX, target, 400) &&
                      vcnWaitReg(kRegPowerStatus, kPowerStatusMask, kPowerStatusTilesOff, 400);
        }
    }
    const uint32_t power = vrd(kRegPowerStatus);
    if (power == UINT32_MAX || power == 0xDEADBEEFu)
        return false;
    vwr(kRegPowerStatus, power & ~kPowerStatusPgMode);
    const uint32_t readback = vrd(kRegPowerStatus);
    const bool cleared = readback != UINT32_MAX && readback != 0xDEADBEEFu && !(readback & kPowerStatusPgMode);
    if (cleared) vcnDpgActive = false;
    return drained && cleared;
}

bool PhxBringup::vcnPowerDown(uint32_t *response) {
    for (auto &session : avcSessions) {
        session.ownerID = 0;
        session.parser.reset();
        releaseAvcTargetMap(session);
    }
    const bool dpgStopped = !vcnDpgActive || vcnStopDpg();
    if (!dpgStopped)
        IOLog("PhxBringup: VCN DPG stop incomplete before SMU power down (wptr %u)\n", vcnRingWptr);
    // Registers other than PGFSM are only safe while the planes are on.
    const bool on = vcnPlanesOn && vcnRegOk(kRegPgfsmStatus) &&
                    (vrd(kRegPgfsmStatus) & kPgfsmStatusMask) == kPgfsmPoweredPg;
    if (on && vcnRegOk(kRegRbEnable)) {
        const uint32_t enabled = vrd(kRegRbEnable);
        if (enabled != UINT32_MAX) {
            vwr(kRegRbEnable, enabled & ~kRbEnableRb1);
            (void)vrd(kRegRbEnable);
        }
    }
    if (on) {
        // vcn_v4_0_enable_static_power_gating: tiles-off indicator first, then every plane off.
        uint32_t ps = vrd(kRegPowerStatus);
        if (ps != UINT32_MAX)
            vwr(kRegPowerStatus, (ps & ~kPowerStatusMask) | kPowerStatusTilesOff);
    }
    if (vcnPlanesOn) {
        vwr(kRegPgfsmConfig, kPgfsmConfigGate);
        if (!vcnWaitReg(kRegPgfsmStatus, kPgfsmStatusMask, kPgfsmGated, 400))
            IOLog("PhxBringup: VCN planes did not gate (PGFSM_STATUS 0x%08x)\n", vrd(kRegPgfsmStatus));
        vcnPlanesOn = false;
    }
    uint32_t after = 0;
    const IOReturn r = vcnSmu(kPpsmcPowerDownVcn, 0, response, &after);
    vcnReady = false;
    vcnRingWptr = 0;
    vcnFwMc = 0;
    const bool smuStopped = r == kIOReturnSuccess && *response == kPpsmcResultOk;
    const bool stopped = smuStopped && dpgStopped;
    bool reserved = false;
    for (auto &session : avcSessions) {
        if (stopped) session.stagingBytes = session.persistentBytes = session.bankBytes = 0;
        reserved |= session.persistentBytes != 0;
    }
    // A failed AVC shutdown must be retried before init or raw VRAM reuse.
    vcnNeedsPowerDown = !smuStopped || reserved;
    publishAvcSessionCount();
    if (!smuStopped)
        IOLog("PhxBringup: VCN PowerDownVcn failed (kr 0x%08x, response 0x%08x)\n", r, *response);
    else
        vcnDpgActive = false;
    return stopped;
}

IOReturn PhxBringup::vcnStatus(uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    out[0] = vcnReady;
    out[4] = vcnFwMc;
    if (vcnReady) {
        out[1] = vcnReadVram(kBoOffset + kSharedOff + kSharedPresentFlags);
        out[2] = vcnReadVram(kBoOffset + kSharedOff + kSharedSq);      // sq: enabled | mode << 8 | status << 16
    }
    vcnRecord(out);
    out[7] |= static_cast<uint64_t>(vcnRingWptr) << 32;
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

// SMU power-up, register programming and VCPU boot. Runs with vcnLock held.
// SOC15_DPG_MODE write: value to UVD_DPG_LMA_DATA, then the internal address to UVD_DPG_LMA_CTL with READ_WRITE set.
void PhxBringup::dpgWrite(uint32_t reg, uint32_t value) {
    if (vcnSram) {   // indirect mode: collect the pair, the PSP applies the whole list at once
        if (vcnSramN + 2u <= sizeof(vcnSramBuf) / sizeof(vcnSramBuf[0])) {
            vcnSramBuf[vcnSramN++] = phx_vcn::dpgModeOffset(reg);
            vcnSramBuf[vcnSramN++] = value;
        }
        return;
    }
    vwr(kRegDpgLmaData, value);
    vwr(kRegDpgLmaCtl, 0x1u | (phx_vcn::dpgModeOffset(reg) << 16));
}

// Register read-back used after a VCPU boot timeout (each read lands in the breadcrumb ring).
void PhxBringup::vcnVcpuDiag() {
    vcnCrumb(0x50000000u | 0x80u, 0);
    for (uint32_t i = 0; i < sizeof(kVcnDiagRegs) / sizeof(kVcnDiagRegs[0]); i++)
        if (vcnRegOk(kVcnDiagRegs[i]))
            (void)vrd(kVcnDiagRegs[i]);
    vcnSweepDump();
}

// Program the VCPU through the DPG port or prepare the equivalent indirect PSP sequence.
uint32_t PhxBringup::vcnDpgStart(uint32_t ucodeSize, uint64_t out[8]) {
    vcnSramN = 0;
    // Measured against a running Linux on this machine: PGFSM stays all-off (0x2aaaaaaa), POWER_STATUS ends at
    // 0x101 (TILES_OFF | PG_EN, no PG_MODE), and UVD_STATUS reads 0xdeadbeef the whole time. So deadbeef is normal
    // here and PGFSM must not be forced on.
    if (vcnSram) {
        // vcn_v4_0_start_dpg_mode as Linux runs it here: a live Linux dump shows POWER_STATUS = 0x105 while VCN is
        // up, i.e. UVD_POWER_STATUS | PG_MODE | PG_EN. PG_MODE was the one bit the earlier attempt never set; the
        // PSP's VCN_RAM load very likely requires the block to be in dynamic power-gating mode.
        uint32_t ps = vrd(kRegPowerStatus);
        if (ps == UINT32_MAX || ps == 0xDEADBEEFu)
            return PHX_VCN_NO_BLOCK;
        vwr(kRegPowerStatus, (ps & ~kPowerStatusMask) | kPowerStatusTilesOff);   // disable the anti-hang mechanism
        ps = vrd(kRegPowerStatus);
        if (ps == UINT32_MAX || ps == 0xDEADBEEFu)
            return PHX_VCN_NO_BLOCK;
        vwr(kRegPowerStatus, ps | kPowerStatusPgMode | kPowerStatusPgEn);
        vcnDpgActive = true;
    } else if (!vcnDpgPgMode) {
        uint32_t ps0 = vrd(kRegPowerStatus);
        if (ps0 == UINT32_MAX || ps0 == 0xDEADBEEFu)
            return PHX_VCN_NO_BLOCK;
        vwr(kRegPowerStatus, (ps0 & ~kPowerStatusMask) | kPowerStatusTilesOff | kPowerStatusPgEn);
    }
    if (vcnDpgPgMode) {
        uint32_t ps = vrd(kRegPowerStatus);
        if (ps == UINT32_MAX || ps == 0xDEADBEEFu)
            return PHX_VCN_NO_BLOCK;
        vwr(kRegPowerStatus, (ps & ~kPowerStatusMask) | kPowerStatusTilesOff);
        ps = vrd(kRegPowerStatus);
        if (ps == UINT32_MAX || ps == 0xDEADBEEFu)
            return PHX_VCN_NO_BLOCK;
        vwr(kRegPowerStatus, (ps & ~kPowerStatusMask) | kPowerStatusTilesOff | kPowerStatusPgMode | kPowerStatusPgEn);
        vcnDpgActive = true;
    }

    // the per-instance block: zero it, then the fw_shared handshake fields
    vcnZeroVram(kBoOffset, kBoSize);
    const uint32_t flags = kFwFlagUnifiedQueue | kFwFlagSmuDpm | kFwFlagDrmKeyInject;
    vcnWriteVram(kBoOffset + kSharedOff + kSharedPresentFlags, flags);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedSq, kSharedSqEnabled);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedSmuInterface, kFwSmuInterfaceApu);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedDrmKey, kFwDrmKeyMethod);
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    if (vcnStopStage == 6) { return PHX_VCN_STOPPED; }

    const auto write = [this](uint32_t reg, uint32_t value) { dpgWrite(reg, value); };
    writeDpgSetup(write, vcnSram, vcnFwMc, firmwareCacheBytes(ucodeSize),
                  vcnMc(kBoOffset + kStackOff), vcnMc(kBoOffset + kCtxOff),
                  vcnMc(kBoOffset + kSharedOff), kGbAddrConfig);
    if (vcnStopStage == 7) { return PHX_VCN_STOPPED; }
    writeDpgRelease(write);

    if (vcnSram) {
        // amdgpu_vcn_psp_update_sram: (offset, value) pairs in VRAM, then LOAD_IP_FW with GFX_FW_TYPE_VCN0_RAM.
        // Optional padding repeats the final write to MASTINT_EN.
        uint32_t n = vcnSramN;
        if (vcnSramTrim && vcnSramTrim * 2u < n)
            n = vcnSramTrim * 2u;                                   // cut the list after pair vcnSramTrim
        const uint32_t padTo = vcnSramFull ? 4096u : vcnSramBytes;   // pairs occupy 8 bytes each
        if (padTo) {
            const uint32_t cap = sizeof(vcnSramBuf) / sizeof(vcnSramBuf[0]);
            while (n * 4u < padTo && n + 2u <= cap) {
                vcnSramBuf[n] = vcnSramBuf[vcnSramN - 2u];
                vcnSramBuf[n + 1u] = vcnSramBuf[vcnSramN - 1u];
                n += 2u;
            }
        }
        const uint32_t sramOff = vcnSramAbs ? vcnSramAbs * 4096u
                               : (vcnSramPage ? kBoOffset + vcnSramPage * 4096u : kBoOffset + kSramOff);
        IOLog("PhxBringup: VCN SRAM list %u pairs at +0x%x\n", n / 2u, sramOff);
        for (uint32_t i = 0; i + 1u < n; i += 2u)
            IOLog("PhxBringup:  sram[%02u] off 0x%05x = 0x%08x\n", i / 2u, vcnSramBuf[i], vcnSramBuf[i + 1u]);
        vcnCrumb(0x50000000u | 0x88u, n);   // dwords sent to the PSP
        vcnCrumb(0x50000000u | 0x8Cu, sramOff);   // byte offset of the block inside the BO
        vcnCrumb(0x50000000u | 0x89u, vcnFwType ? vcnFwType : kPspFwTypeVcnRam);   // fw_type actually used
        for (uint32_t i = 0; i < n; i++)
            vcnWriteVram(sramOff + i * 4u, vcnSramBuf[i]);
        __asm__ __volatile__("sfence" ::: "memory");
        hdpFlush();
        const uint64_t src = vcnMc(sramOff);
        const uint32_t fwType = vcnFwType ? vcnFwType : kPspFwTypeVcnRam;
        const uint32_t u[4] = {static_cast<uint32_t>(src), static_cast<uint32_t>(src >> 32),
                               n * 4u, fwType};
        uint64_t pspOut[4] = {};
        IOLockLock(smuLock);
        const IOReturn r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, pspOut);
        IOLockUnlock(smuLock);
        vcnCrumb(0x50000000u | 0x87u, static_cast<uint32_t>(pspOut[0]));   // 0 = applied
        vcnCrumb(0x50000000u | 0x8Au, static_cast<uint32_t>(pspOut[2]));   // resp.fw_addr_lo
        vcnCrumb(0x50000000u | 0x8Bu, static_cast<uint32_t>(pspOut[3]));   // resp.fw_addr_hi
        if (r != kIOReturnSuccess || pspOut[0] != 0) {
            out[1] = pspOut[0];
            if (!vcnPspIgnore)
                return PHX_VCN_PSP;
        }
    }

    // Normal startup continues directly to ring setup, as the Linux DPG path does.
    // Stage 8 requests a firmware boot check before any queue traffic.
    if (vcnStopStage != 8)
        return PHX_VCN_OK;
    uint32_t scratch = 0;
    for (uint32_t i = 0; i < 2000u; i++) {
        scratch = vrd(kRegScratch1);
        if (scratch && scratch != UINT32_MAX && scratch != 0xDEADBEEFu) {
            vcnCrumb(0x50000082u, scratch);
            return PHX_VCN_STOPPED;
        }
        IOSleep(1);
    }
    vcnCrumb(0x50000082u, scratch);
    return PHX_VCN_NO_VCPU;
}

IOReturn PhxBringup::vcnSetDoorbell(uint32_t index) {
    if (!vcnLock)
        return kIOReturnNotReady;
    if (index && (index < kDbRangeStart || index >= kDbRangeStart + kDbRangeCount))
        return kIOReturnBadArgument;
    IOLockLock(vcnLock);
    const bool busy = vcnNeedsPowerDown;
    if (!busy)
        vcnDbIndex = index ? index : kDbIndexLinux;
    IOLockUnlock(vcnLock);
    return busy ? kIOReturnBusy : kIOReturnSuccess;
}

// NBIO 7.7.0 exposes the global aperture directly and the per-VCN route through RSMU.
bool PhxBringup::vcnConfigureDoorbell(uint64_t out[8]) {
    if (!doorbellMap || (vcnDbIndex + 1u) * 4u > doorbellMap->getLength() ||
        !vcnRegOk(kDoorbellApertureReg) || !vcnRegOk(kRsmuData))
        return false;
    uint32_t aperture = vrd(kDoorbellApertureReg);
    if (aperture != UINT32_MAX && aperture != 0xDEADBEEFu && !(aperture & 1u)) {
        vwr(kDoorbellApertureReg, aperture | 1u);
        aperture = vrd(kDoorbellApertureReg);
    }
    out[1] = aperture;
    if (aperture == UINT32_MAX || aperture == 0xDEADBEEFu || !(aperture & 1u))
        return false;
    vwr(kRsmuIndex, kVcnDoorbellRangeAddr);
    out[1] = vrd(kRsmuIndex);
    if (out[1] != kVcnDoorbellRangeAddr)
        return false;
    uint32_t route = vrd(kRsmuData);
    if (route != UINT32_MAX && route != 0xDEADBEEFu && (route & kVcnDoorbellFields) != kVcnDoorbellRange) {
        vwr(kRsmuData, (route & ~kVcnDoorbellFields) | kVcnDoorbellRange);
        route = vrd(kRsmuData);
    }
    out[1] = route;
    return (route & kVcnDoorbellFields) == kVcnDoorbellRange;
}

uint32_t PhxBringup::vcnStaticStart(uint32_t ucodeSize) {
    // vcn_v4_0_disable_static_power_gating, AMD_PG_SUPPORT_VCN branch. Nothing but PGFSM may be accessed until
    // the planes report on: an access to the powered-off block is a fatal fabric error (instant reset).
    vwr(kRegPgfsmConfig, kPgfsmConfigPg);
    vcnPlanesOn = true;   // from here a power-down gates the planes first, even if the wait below fails
    if (!vcnWaitReg(kRegPgfsmStatus, kPgfsmStatusMask, kPgfsmPoweredPg, 400)) {
        return PHX_VCN_NO_POWER;
    }
    vcnCrumb(0x50000000u | 3u, 0);
    if (vcnStopStage == 3) { return PHX_VCN_STOPPED; }   // + PGFSM planes on
    uint32_t powerStatus = vrd(kRegPowerStatus);
    if (powerStatus == UINT32_MAX) {
        return PHX_VCN_NO_BLOCK;
    }
    powerStatus &= kPowerStatusKeep;
    powerStatus |= kPowerStatusPgOn;
    vwr(kRegPowerStatus, powerStatus);
    // Linux programs POWER_STATUS before reading UVD_STATUS. Until then the register can report 0xdeadbeef.
    if (vrd(kRegUvdStatus) == UINT32_MAX) {
        return PHX_VCN_NO_BLOCK;
    }
    vcnCrumb(0x50000000u | 4u, 0);
    if (vcnStopStage == 4) { return PHX_VCN_STOPPED; }   // + POWER_STATUS, UVD_STATUS readable

    // 2. the non-DPG start sequence of vcn_v4_0_start (clock gating is left to the SMU: this block runs
    //    VCN_MGCG, where Linux returns before touching the CGC registers).
    vwr(kRegRbEnable, vrd(kRegRbEnable) & ~kRbEnableRb1);
    vwr(kRegUvdStatus, kUvdStatusBusy);
    vcnDisableClockGating();            // Linux order: status busy, clock gating, then the VCPU clock
    vwr(kRegVcpuCntl, kVcpuCntlHold);   // literal: reserved bits read as 1 and must not be written back
    (void)vrd(kRegVcpuCntl);
    vwr(kRegMastintEn, vrd(kRegMastintEn) & ~kMastintEnVcpuEn);
    vwr(kRegLmiCtrl2, vrd(kRegLmiCtrl2) & ~kLmiCtrl2StallUmc);
    vwr(kRegSoftReset, vrd(kRegSoftReset) & ~(kSoftResetLmi | kSoftResetLmiUmc));
    vwr(kRegLmiCtrl, vrd(kRegLmiCtrl) | kLmiCtrlFlags);
    vwr(kRegMpcCntl, (vrd(kRegMpcCntl) & ~kMpcCntlReplMask) | (kMpcCntlReplMode << kMpcCntlReplShift));
    vwr(kRegMpcSetMuxA0, kMpcSetMuxA);
    vwr(kRegMpcSetMuxB0, kMpcSetMuxB);
    vwr(kRegMpcSetMux, kMpcSetMux);
    vcnCrumb(0x50000000u | 5u, 0);
    if (vcnStopStage == 5) { return PHX_VCN_STOPPED; }   // + start-sequence registers

    // 3. the per-instance VRAM block: stack, context, fw_shared and ring. Zero it, then write the fw_shared
    //    handshake fields exactly as vcn_v4_0_fw_shared_init does (v4.0.2 also wants the DRM-key workaround).
    vcnZeroVram(kBoOffset, kBoSize);
    const uint32_t flags = kFwFlagUnifiedQueue | kFwFlagSmuDpm | kFwFlagDrmKeyInject;
    vcnWriteVram(kBoOffset + kSharedOff + kSharedPresentFlags, flags);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedSq, kSharedSqEnabled);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedSmuInterface, kFwSmuInterfaceApu);
    vcnWriteVram(kBoOffset + kSharedOff + kSharedDrmKey, kFwDrmKeyMethod);
    // The physical function leaves SR-IOV ring metadata zeroed.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();

    vcnCrumb(0x50000000u | 6u, 0);
    if (vcnStopStage == 6) { return PHX_VCN_STOPPED; }   // + VRAM block zeroed, fw_shared written

    // 4. VCPU cache windows: 0 = firmware in the TMR, 1 = stack, 2 = context, NC0 = fw_shared.
    const uint32_t cache0 = firmwareCacheBytes(ucodeSize);
    const uint64_t stackMc = vcnMc(kBoOffset + kStackOff);
    const uint64_t ctxMc = vcnMc(kBoOffset + kCtxOff);
    const uint64_t sharedMc = vcnMc(kBoOffset + kSharedOff);
    vwr(kRegLmiVcpuCacheBarLo, static_cast<uint32_t>(vcnFwMc));
    vwr(kRegLmiVcpuCacheBarHi, static_cast<uint32_t>(vcnFwMc >> 32));
    vwr(kRegVcpuCacheOffset0, 0);
    vwr(kRegVcpuCacheSize0, cache0);
    vwr(kRegLmiVcpuCache1BarLo, static_cast<uint32_t>(stackMc));
    vwr(kRegLmiVcpuCache1BarHi, static_cast<uint32_t>(stackMc >> 32));
    vwr(kRegVcpuCacheOffset1, 0);
    vwr(kRegVcpuCacheSize1, kStackSize);
    vwr(kRegLmiVcpuCache2BarLo, static_cast<uint32_t>(ctxMc));
    vwr(kRegLmiVcpuCache2BarHi, static_cast<uint32_t>(ctxMc >> 32));
    vwr(kRegVcpuCacheOffset2, 0);
    vwr(kRegVcpuCacheSize2, kCtxSize);
    vwr(kRegLmiVcpuNc0BarLo, static_cast<uint32_t>(sharedMc));
    vwr(kRegLmiVcpuNc0BarHi, static_cast<uint32_t>(sharedMc >> 32));
    vwr(kRegVcpuNoncacheOffset0, 0);
    vwr(kRegVcpuNoncacheSize0, kSharedSize);
    vwr(kRegGfx10AddrConfig, kGbAddrConfig);   // GB_ADDR_CONFIG of this APU
    vcnCrumb(0x50000000u | 7u, 0);
    if (vcnStopStage == 7) { return PHX_VCN_STOPPED; }   // + cache windows (VCPU still in reset)

    // 5. let the VCPU run and wait for its firmware-ready report (UVD_STATUS bit 1).
    vwr(kRegRbArbCtrl, vrd(kRegRbArbCtrl) & ~kRbArbCtrlVcpuDis);
    vwr(kRegVcpuCntl, kVcpuCntlRun);    // literal, as above: releases BLK_RST only
    (void)vrd(kRegVcpuCntl);            // read-back lands in the breadcrumb ring
    // vcn_v4_0_start retries the VCPU ten times: 1 s per try, holding it in reset for 10 ms between tries.
    bool vcpuReady = false;
    for (uint32_t attempt = 0; attempt < 10u && !vcpuReady; attempt++) {
        vcpuReady = vcnWaitReg(kRegUvdStatus, kUvdStatusVcpuRdy, kUvdStatusVcpuRdy, 1000);
        if (vcpuReady)
            break;
        vwr(kRegVcpuCntl, kVcpuCntlHold);
        IOSleep(10);
        vwr(kRegVcpuCntl, kVcpuCntlRun);
        IOSleep(10);
    }
    // Recorded before the outcome is known: on failure this is the only evidence of whether the VCPU ran.
    vcnCrumb(0x50000000u | 0x82u, vrd(kRegScratch1));
    if (!vcpuReady) {
        vcnVcpuDiag();
        return PHX_VCN_NO_VCPU;
    }
    {
        const uint32_t st = vrd(kRegUvdStatus);
        if (st != UINT32_MAX && st != 0xDEADBEEFu)
            vwr(kRegUvdStatus, st & ~kUvdStatusBusy);
    }
    vcnCrumb(0x50000000u | 8u, 0);
    if (vcnStopStage == 8) { return PHX_VCN_STOPPED; }   // + VCPU booted and reported ready
    return PHX_VCN_OK;
}

uint32_t PhxBringup::vcnSetupRing() {
    const uint32_t dbCtrl = (vcnDbIndex << kRb1DbCtrlOffsetShift) | kRb1DbCtrlEn;
    if (!vcnDpg)
        vwr(kRegRb1DbCtrl, dbCtrl);
    const uint64_t vcnRingMc = vcnMc(kBoOffset + kRingOff);
    vwr(kRegRbBaseLo, static_cast<uint32_t>(vcnRingMc));
    vwr(kRegRbBaseHi, static_cast<uint32_t>(vcnRingMc >> 32));
    vwr(kRegRbSize, kRingDw);
    uint32_t enabled = vrd(kRegRbEnable);
    if (enabled == UINT32_MAX || enabled == 0xDEADBEEFu)
        return PHX_VCN_RING_NOT_ENABLED;
    vwr(kRegRbEnable, enabled & ~kRbEnableRb1);
    vcnSharedQueueMode(kFwQueueRingReset, 0);
    vwr(kRegRbRptr, 0);
    vwr(kRegRbWptr, 0);
    if (vrd(kRegRbRptr) != 0 || vrd(kRegRbWptr) != 0)
        return PHX_VCN_WPTR_REJECTED;
    vcnRingWptr = 0;
    vwr(kRegRbEnable, enabled | kRbEnableRb1);
    vcnSharedQueueMode(0, kFwQueueRingReset | kFwQueueDpgHoldOff);
    if (vcnDpg)
        vwr(kRegRb1DbCtrl, dbCtrl);
    enabled = vrd(kRegRbEnable);
    if (enabled == UINT32_MAX || enabled == 0xDEADBEEFu || !(enabled & kRbEnableRb1))
        return PHX_VCN_RING_NOT_ENABLED;
    return PHX_VCN_OK;
}

uint32_t PhxBringup::vcnPowerUp(uint32_t ucodeSize, uint64_t out[8]) {
    uint32_t resp = 0, after = 0;
    const IOReturn r = vcnSmu(kPpsmcPowerUpVcn, 0, &resp, &after);
    out[1] = resp;
    if (r != kIOReturnSuccess || resp != kPpsmcResultOk)
        return PHX_VCN_SMU;
    vcnNeedsPowerDown = true;
    vcnCrumb(0x50000002u, 0);
    uint32_t code;
    if (vcnStopStage == 2)
        code = PHX_VCN_STOPPED;
    else if (!vcnConfigureDoorbell(out))
        code = PHX_VCN_DB_ROUTE;
    else
        code = vcnDpg ? vcnDpgStart(ucodeSize, out) : vcnStaticStart(ucodeSize);
    if (code == PHX_VCN_OK)
        code = vcnSetupRing();
    if (code == PHX_VCN_OK) {
        vcnReady = true;
        uint32_t before = 0, afterFence = 0, observed = 0;
        code = vcnSubmitFence(&before, &afterFence, &observed);
        if (code != PHX_VCN_OK)
            out[1] = (static_cast<uint64_t>(vcnFenceSeq) << 32) | observed;
    }
    vcnRecord(out);
    if (code != PHX_VCN_OK)
        vcnPowerDown(&resp);
    return code;
}

// Caller holds smuLock so the TMR and fw_buf cannot change during validation or PSP submission.
uint32_t PhxBringup::vcnLoadFirmware(uint32_t fwOff, uint32_t fwLen, uint64_t preloadMc, uint64_t out[8]) {
    if (!ringMc || !fwBufSize || !tmrSetUp)
        return PHX_VCN_NOT_READY;
    if ((fwOff & 3u) || fwLen < sizeof(FwHeader) || static_cast<uint64_t>(fwOff) + fwLen > fwBufSize ||
        (tmrOff < kBoOffset + kBoSize && tmrOff + tmrSize > kBoOffset))
        return PHX_VCN_BAD_ARG;

    uint32_t words[sizeof(FwHeader) / sizeof(uint32_t)];
    for (uint32_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        words[i] = vcnReadVram(static_cast<uint32_t>(fwBufOff + fwOff + i * sizeof(uint32_t)));
    FwHeader h;
    memcpy(&h, words, sizeof(h));
    uint32_t ucodeOff = 0, ucodeSize = 0;
    if (!fwHeaderOk(h, fwLen, &ucodeOff, &ucodeSize))
        return PHX_VCN_BAD_FW;
    out[2] = ucodeOff;
    out[3] = ucodeSize;

    uint64_t fwMc = preloadMc;
    if (!fwMc) {
        const uint64_t src = fwBufMc + fwOff + ucodeOff;
        const uint32_t u[4] = {static_cast<uint32_t>(src), static_cast<uint32_t>(src >> 32),
                               ucodeSize, kPspFwTypeVcn};
        uint64_t pspOut[4] = {};
        const IOReturn r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, pspOut);
        out[1] = pspOut[0];
        if (r != kIOReturnSuccess || pspOut[0] != 0)
            return PHX_VCN_PSP;
        fwMc = (pspOut[3] << 32) | pspOut[2];
        if (!fwMc)
            return PHX_VCN_NO_FW_ADDR;
    }
    out[4] = fwMc;
    if (!firmwareFitsTmr(fwMc, firmwareCacheBytes(ucodeSize), tmrMc, tmrSize))
        return preloadMc ? PHX_VCN_BAD_ARG : PHX_VCN_NO_FW_ADDR;
    vcnFwMc = fwMc;
    return PHX_VCN_OK;
}


// LOAD_TOC (GFX_CMD_ID_LOAD_TOC): hand the PSP the firmware table of contents. Linux sends this once during PSP
// bring-up, before any IP firmware; the PSP uses it to know which IP slots exist and to size the TMR. Our bring-up
// never sent it, which is the prime suspect for the VCN_RAM (type 49) load being refused.
IOReturn PhxBringup::vcnLoadToc(uint32_t fwOff, uint32_t size, uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock || !smuLock)
        return kIOReturnNotReady;
    if (!ringMc || !fwBufSize || size == 0 || (fwOff & 3u) ||
        static_cast<uint64_t>(fwOff) + static_cast<uint64_t>(size) > fwBufSize)
        return kIOReturnBadArgument;
    const uint64_t mc = fwBufMc + fwOff;
    const uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, 0};
    IOLockLock(smuLock);
    const IOReturn r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_TOC, u, 3, out);
    IOLockUnlock(smuLock);
    IOLog("PhxBringup: VCN LOAD_TOC %u bytes at 0x%llx -> status 0x%llx (kr 0x%x)\n", size,
          (unsigned long long)mc, (unsigned long long)out[0], r);
    return r;
}

IOReturn PhxBringup::vcnInit(uint32_t fwOff, uint32_t fwLen, uint64_t preloadMc, uint32_t stopStage,
                             uint64_t sramBytes, uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    if (isSleeping) {
        out[0] = PHX_VCN_NOT_READY;
        IOLockUnlock(vcnLock);
        return kIOReturnSuccess;
    }
    vcnCrumbMode = (stopStage >> 8) & 0x3u;   // bits 9:8 of the stage word (op bits 17:16)
    vcnDpg = ((stopStage >> 10) & 0x1u) != 0; // bit 10 (op bit 18): the DPG start path
    vcnDpgPgMode = ((stopStage >> 11) & 0x1u) != 0;   // bit 11 (op bit 19): also enable POWER_STATUS PG mode
    vcnSram = ((stopStage >> 12) & 0x1u) != 0;        // bit 12 (op bit 20): stage the sequence through the PSP
    vcnSramFull = ((stopStage >> 13) & 0x1u) != 0;    // bit 13 (op bit 21): send all 512 bytes of the block
    vcnFwType = (stopStage >> 16) & 0xFFu;            // op bits 31:24 land in stopStage bits 23:16: LOAD_IP_FW type
    const uint32_t sramSize = static_cast<uint32_t>(sramBytes & 0xFFFFFFFFu);
    vcnSramBytes = (sramSize >= 8u && sramSize <= 8192u && !(sramSize & 7u)) ? sramSize : 0u;
    vcnPspIgnore = ((sramBytes >> 20) & 0x1u) != 0;   // scrollInput[5] bit 20: --psp-ignore
    vcnSramPage = (sramBytes >> 24) & 0xFFu;          // bits 31:24: which 4 KB page of the BO holds the block
    vcnSramTrim = static_cast<uint32_t>((sramBytes >> 32) & 0xFFFFu);   // bits 47:32: send only the first N pairs
    vcnSramAbs = static_cast<uint32_t>((sramBytes >> 48) & 0xFFFFu);    // bits 63:48: absolute aperture page
    stopStage &= 0xFFu;
    vcnCrumbReset();
    uint32_t code = PHX_VCN_OK;
    vcnRecord(out);
    if (vcnReady) {
        code = PHX_VCN_ALREADY;
        out[4] = vcnFwMc;
    } else if (fbAperLen < static_cast<uint64_t>(kBoOffset) + kBoSize ||
               fbSize < static_cast<uint64_t>(kBoOffset) + kBoSize || !vcnRegOk(kRegLmiCtrl)) {
        // LMI_CTRL has the largest MMIO offset used in this sequence.
        code = PHX_VCN_BAD_ARG;
    } else {
        uint32_t response = 0;
        if (vcnNeedsPowerDown && !vcnPowerDown(&response)) {
            code = PHX_VCN_POWERDOWN;
            out[1] = response;
        } else {
            IOLockLock(smuLock);
            code = vcnLoadFirmware(fwOff, fwLen, preloadMc, out);
            IOLockUnlock(smuLock);
            vcnStopStage = stopStage;
            if (code == PHX_VCN_OK && stopStage == 1) {
                code = PHX_VCN_STOPPED;   // PSP load only: VCN was never powered
                vcnFwMc = 0;
            } else if (code == PHX_VCN_OK) {
                code = vcnPowerUp(static_cast<uint32_t>(out[3]), out);
                if (code != PHX_VCN_OK)
                    vcnFwMc = 0;
            }
        }
    }
    out[0] = static_cast<uint64_t>(code);
    out[7] |= static_cast<uint64_t>(vcnRingWptr) << 32;
    vcnStopStage = 0;
    vcnCrumbMode = 0;
    vcnDpg = false;
    vcnDpgPgMode = false;
    vcnSram = false;
    vcnSramFull = false;
    vcnSramBytes = 0;
    vcnSramPage = 0;
    vcnSramTrim = 0;
    vcnSramAbs = 0;
    vcnPspIgnore = false;
    vcnFwType = 0;
    vcnSramN = 0;
    IOLog("PhxBringup: VCN init (stop stage %u) code %u (detail 0x%llx, firmware MC 0x%llx, status 0x%08x)\n", stopStage, code,
          (unsigned long long)out[1], (unsigned long long)out[4], static_cast<uint32_t>(out[5]));
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

// The diagnostic queue has one producer and one synchronous batch in flight. Wait for END and fetch padding
// before reusing the ring. The Linux unified ring wraps its 32-bit write pointer at the ring size.
uint32_t PhxBringup::vcnSubmitTest(const uint32_t *packet, uint32_t count, uint32_t seq,
                                   uint32_t *before, uint32_t *after, uint32_t *observed,
                                   uint32_t timeoutMs, bool hot, bool pFrame) {
    if (!vcnReady || isSleeping)
        return PHX_VCN_NOT_READY;
    const uint32_t enabled = vrd(kRegRbEnable);
    if (enabled == UINT32_MAX || enabled == 0xDEADBEEFu || !(enabled & kRbEnableRb1))
        return PHX_VCN_RING_NOT_ENABLED;
    *before = *after = vrd(kRegRbRptr);
    if (*before != vcnRingWptr)
        return PHX_VCN_RPTR_STUCK;

    const uint32_t ringBase = kBoOffset + kRingOff;
    const uint32_t committed = ringCommitAfter(vcnRingWptr, count);
    for (uint32_t i = 0; i < committed - vcnRingWptr; i++) {
        const uint32_t slot = ringSlot(vcnRingWptr + i);
        vcnWriteVram(ringBase + slot * 4u, i < count ? packet[i] : kCmdNop);
    }
    if (seq)
        vcnWriteVram(kBoOffset + kWbOff, 0);
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    vcnRingWptr = ringSlot(committed);
    reinterpret_cast<volatile uint32_t *>(doorbellMap->getVirtualAddress())[vcnDbIndex] = vcnRingWptr;
    // The decode path (hot) retires inside a fraction of a millisecond; the diagnostic paths keep the
    // bring-up loop so their behaviour does not change with debug.phx_vcn_wait.
    if (hot && gVcnWait > 0)
        return vcnWaitFence(vcnRingWptr, seq, timeoutMs, after, observed, pFrame);
    for (uint32_t i = 0; i < timeoutMs; i++) {
        *after = vrd(kRegRbRptr);
        if (seq)
            *observed = vcnReadVram(kBoOffset + kWbOff);
        if (*after == vcnRingWptr && (!seq || *observed == seq))
            return PHX_VCN_OK;
        IOSleep(1);
    }
    if (seq && *observed != seq)
        return PHX_VCN_FENCE_TIMEOUT;
    return PHX_VCN_RPTR_STUCK;
}

// Spin briefly, then sleep in sub-millisecond slices, then fall back to the bring-up loop. The hardware
// normally retires before the first IOSleep(1) tick ends, so the tick was most of the cost (see PhxVcnWait.h
// for the state machine and tests/vcn_host_test.cpp for its host test).
uint32_t PhxBringup::vcnWaitFence(uint32_t wptr, uint32_t seq, uint32_t timeoutMs, uint32_t *after, uint32_t *observed,
                                  bool updateEwma) {
    // Only a P frame teaches the average: an IDR or a session command is not a
    // useful sample, and the caller decides rather than a flag a failed submit
    // could leave armed.
    phx_vcn::VcnWaitParams params{};
    params.mode = gVcnWait >= 2 ? 2u : 1u;
    params.timeoutMs = timeoutMs ? timeoutMs : 1u;
    const int slice = gVcnSliceUs < 10 ? 10 : (gVcnSliceUs > 1000 ? 1000 : gVcnSliceUs);
    params.sliceUs = static_cast<uint32_t>(slice);
    const int spin = gVcnSpinMaxUs < 0 ? 0 : (gVcnSpinMaxUs > 2000 ? 2000 : gVcnSpinMaxUs);
    params.spinMaxNs = static_cast<uint64_t>(spin) * 1000ull;
    const SInt64 ewma = gVcnHwEwmaNs;
    params.hwEwmaNs = ewma > 0 ? static_cast<uint64_t>(ewma) : 1000000ull;
    const auto nowNs = []() -> uint64_t {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        return ns;
    };
    const auto probe = [this, wptr, seq, after, observed]() -> bool {
        *after = vrd(kRegRbRptr);
        if (seq)
            *observed = vcnReadVram(kBoOffset + kWbOff);
        return *after == wptr && (!seq || *observed == seq);
    };
    const auto pause = []() { __asm__ __volatile__("pause"); };
    const auto shortSleep = [this](uint64_t ns) {
        uint64_t us = ns / 1000ull;
        if (us < 1) us = 1;
        if (us > 1000000ull) us = 1000000ull;
        AbsoluteTime deadline = 0;
        clock_interval_to_deadline(static_cast<uint32_t>(us), kMicrosecondScale, &deadline);
        if (!deadline) {
            IOSleep(1);   // no deadline available: keep one tick instead of spinning the stage
            return;
        }
        IOLockLock(vcnWaitLock);
        IOLockSleepDeadline(vcnWaitLock, &vcnWaitEvt, deadline, THREAD_UNINT);
        IOLockUnlock(vcnWaitLock);
    };
    const auto sleepTick = []() { IOSleep(1); };
    const phx_vcn::VcnWaitResult result =
        phx_vcn::vcnWaitFence(params, nowNs, probe, pause, shortSleep, sleepTick, updateEwma);
    vcnLastWaitSleeps = result.sleeps;
    vcnLastWaitSpinUs = static_cast<uint32_t>(result.spinNs / 1000ull);
    vcnLastWaitHit = result.hit;
    vcnLastWaitMode = params.mode;
    vcnWaitStats.sleeps += result.sleeps;
    if (result.hit == 1) ++vcnWaitStats.spinHits;
    else if (result.hit == 2) ++vcnWaitStats.shortHits;
    else if (result.hit == 3) ++vcnWaitStats.fallbackHits;
    if (result.elapsedNs > vcnWaitStats.maxNs) vcnWaitStats.maxNs = result.elapsedNs;
    if (updateEwma && result.hit) gVcnHwEwmaNs = static_cast<SInt64>(result.hwEwmaNs);
    if (result.hit) return PHX_VCN_OK;
    return seq && *observed != seq ? PHX_VCN_FENCE_TIMEOUT : PHX_VCN_RPTR_STUCK;
}

IOReturn PhxBringup::vcnRingTest(uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    uint32_t before = 0, after = 0, observed = 0;
    out[0] = vcnSubmitTest(&kCmdEnd, 1, 0, &before, &after, &observed);
    out[1] = before;
    out[2] = after;
    out[3] = vcnRingWptr;
    vcnRecord(out);
    out[7] |= static_cast<uint64_t>(vcnRingWptr) << 32;
    if (vcnReady) {
        out[4] = (vcnReadVram(kBoOffset + kSharedOff + kSharedSq) >> 16) & 0xFFu;
        if (out[0] != PHX_VCN_OK) {
            uint32_t response = 0;
            vcnPowerDown(&response);
        }
    }
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

uint32_t PhxBringup::vcnSubmitFence(uint32_t *before, uint32_t *after, uint32_t *observed) {
    if (++vcnFenceSeq == 0)
        ++vcnFenceSeq;
    uint32_t packet[kFenceDwords];
    buildFencePacket(packet, vcnMc(kBoOffset + kWbOff), vcnFenceSeq);
    return vcnSubmitTest(packet, kFenceDwords, vcnFenceSeq, before, after, observed);
}

IOReturn PhxBringup::vcnFenceTest(uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    uint32_t before = 0, after = 0, observed = 0;
    out[0] = vcnSubmitFence(&before, &after, &observed);
    out[1] = vcnFenceSeq;
    out[2] = observed;
    out[4] = before;
    out[5] = after;
    uint64_t regs[8] = {};
    vcnRecord(regs);
    out[6] = regs[5];
    out[7] = regs[6];
    if (vcnReady && out[0] != PHX_VCN_OK) {
        uint32_t response = 0;
        vcnPowerDown(&response);
    }
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

// Caller holds vcnLock until the ring and the writeback fence have retired.
bool PhxBringup::vcnSubmitCodecIb(uint32_t ibDw, uint32_t phase, uint64_t out[8], bool pFrame) {
    out[1] = phase;
    if (!ibDw || ibDw > sizeof(vcnIb) / sizeof(vcnIb[0])) {
        out[0] = PHX_VCN_BAD_ARG;
        out[2] = out[3] = out[4] = out[5] = 0;
        return false;
    }
    const uint32_t ibOff = kBoOffset + kCodecIbOff;
    for (uint32_t i = 0; i < ibDw; ++i)
        vcnWriteVram(ibOff + i * 4u, vcnIb[i]);
    if (++vcnFenceSeq == 0)
        ++vcnFenceSeq;
    uint32_t packet[5u + kFenceDwords];
    buildIbFencePacket(packet, vcnMc(ibOff), ibDw, vcnMc(kBoOffset + kWbOff), vcnFenceSeq);
    uint32_t before = 0, after = 0, observed = 0;
    out[0] = vcnSubmitTest(packet, 5u + kFenceDwords, vcnFenceSeq, &before, &after, &observed, 10000u, true, pFrame);
    out[2] = vcnFenceSeq;
    out[3] = observed;
    out[4] = before;
    out[5] = after;
    return out[0] == PHX_VCN_OK;
}

IOReturn PhxBringup::vcnCodecSessionTest(bool encode, uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!vcnLock || !mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    if (!vcnReady || isSleeping) {
        out[0] = PHX_VCN_NOT_READY;
    } else {
        const uint32_t dataOff = kBoOffset + kCodecDataOff;
        const uint64_t dataMc = vcnMc(dataOff);
        if (encode)
            vcnZeroVram(dataOff, kEncodeContextBytes);
        for (uint32_t phase = 1; phase <= 2; ++phase) {
            uint32_t ibDw;
            if (encode) {
                ibDw = buildEncodeSessionIb(vcnIb, dataMc, phase == 2);
            } else {
                vcnZeroVram(dataOff, 4096);
                const uint32_t msgDw = buildDecodeSessionMsg(vcnIb, phase == 2);
                for (uint32_t i = 0; i < msgDw; ++i)
                    vcnWriteVram(dataOff + i * 4u, vcnIb[i]);
                ibDw = buildDecodeSessionIb(vcnIb, dataMc);
            }
            if (!vcnSubmitCodecIb(ibDw, phase, out)) {
                uint32_t response = 0;
                vcnPowerDown(&response);
                break;
            }
        }
    }
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

// Caller holds vcnLock and has checked the device is awake. Queue adoption and raw TMR setup also
// reserve the fixed offsets so a concurrent operation cannot claim them after this check returns.
bool PhxBringup::vcnFrameMemoryOk(uint32_t scratchBytes, bool checkDpbBanks) const {
    static_assert(kBoOffset + kBoSize <= kCrumbOff && kCrumbOff + (4u + 2u * kCrumbN) * 4u <= kSweepOff &&
                  kSweepOff + 16u + kSweepCount * 4u <= kDecodeFrameScratchOffset, "VCN regions overlap");
    const uint64_t last = scratchBytes ? kDecodeFrameScratchOffset + scratchBytes : kBoOffset + kBoSize;
    if (!gpuLock || !smuLock || !pci || scratchBytes > kVcnReservedScratch ||
        last > fbAperLen || last > fbSize || fbMcBase > UINT64_MAX - last ||
        kAvcContextBankOffset + kAvcContextPoolBytes > fbAperLen ||
        static_cast<uint64_t>(kAvcDpbBankOffset) + kAvcDpbPoolBytes > fbSize)
        return false;

    IOLockLock(smuLock);
    const auto overlaps = [scratchBytes, checkDpbBanks](uint64_t start, uint64_t length, uint64_t base = 0) {
        return vcnFrameOverlap(start, length, scratchBytes, base, checkDpbBanks);
    };
    bool ok = tmrSetUp && !overlaps(tmrOff, tmrSize) && !overlaps(fwBufOff, fwBufSize);
    IOLockUnlock(smuLock);
    IOLockLock(gpuLock);
    for (const auto &q : gq) {
        if (q.ready && (overlaps(q.ringOff, q.ringDw * 4ull) ||
                        overlaps(q.wbOff, 32) || overlaps(q.fenceOff, 8)))
            ok = false;
    }
    IOLockUnlock(gpuLock);
    if (!ok)
        return false;

    // The boot surface remains live while a flip is showing PhxAMDFB's back buffer. Query its
    // stable physical address from the framebuffer; PE may report a kernel virtual address instead.
    PE_Video video{};
    IOPlatformExpert *platform = getPlatform();
    if (!platform || platform->getConsoleInfo(&video) != kIOReturnSuccess || !video.v_rowBytes ||
        !video.v_height || video.v_height > UINT64_MAX / video.v_rowBytes)
        return false;
    uint64_t bytes = video.v_rowBytes * video.v_height;
    uint64_t frontPhys = video.v_baseAddr & ~static_cast<uint64_t>(PAGE_MASK);
    OSIterator *children = pci->getChildIterator(gIOServicePlane);
    if (!children)
        return false;
    while (OSObject *child = children->getNextObject()) {
        IOService *fb = OSDynamicCast(IOService, child);
        if (!fb || !fb->metaCast("PhxAMDFB"))
            continue;
        NVRMVramRequest scanout{};
        scanout.version = NVRM_VRAM_ABI_VERSION;
        ok = fb->callPlatformFunction(NVRM_VRAM_FN_SCANOUT, false, &scanout, nullptr, nullptr, nullptr) ==
             kIOReturnSuccess && scanout.actualSize && scanout.phys >= fbAperPhys &&
             rangeFits(scanout.phys - fbAperPhys, scanout.actualSize, fbAperLen);
        frontPhys = scanout.phys;
        bytes = scanout.actualSize;
        break;
    }
    children->release();
    if (!ok || overlaps(frontPhys, bytes, fbAperPhys))
        return false;

    // The firmware-only display runs on the primary surface. PhxAMDFB can also point HUBP0 at a
    // second surface, which its allocator places outside BAR0. Check the programmed address too.
    constexpr uint32_t addrLo = 0x34c0u + 0x060au, addrHi = addrLo + 1;
    if (!vcnRegOk(addrHi))
        return false;
    const uint64_t scanoutMc = (static_cast<uint64_t>(rreg(addrHi) & 0xffffu) << 32) | rreg(addrLo);
    return scanoutMc >= fbMcBase && rangeFits(scanoutMc - fbMcBase, bytes, fbSize) &&
           !overlaps(scanoutMc, bytes, fbMcBase);
}

bool PhxBringup::vcnEncoderReady() const {
    if (!vcnLock || !mmio || !fbAper)
        return false;
    IOLockLock(vcnLock);
    const bool ready = vcnReady && !isSleeping && vcnFrameMemoryOk(0);
    IOLockUnlock(vcnLock);
    return ready;
}

// The two stream handles differ from the diagnostic session and single-frame tests.
// Each slot owns its reference surfaces across submissions. Frame staging is reusable
// only after its ring and fence retire and the caller has copied the visible output.
static constexpr uint32_t kAvcFirstHandle = 0x50485844u;

// Place a new session's persistent area and, when the DPB is separate, its reference pictures. Ranges still
// held by any other session (including ones kept reserved after a failed shutdown) are never handed out.
// Caller holds vcnLock; the session being (re)built must already be closed (its ranges are zero).
bool PhxBringup::allocAvcMemory(uint32_t slot, const AvcStreamGeometry &g) {
    phx_vcn::AvcRange ctx[kAvcStreamSlotCount]{}, dpb[kAvcStreamSlotCount]{};
    for (uint32_t i = 0; i < kAvcStreamSlotCount; ++i) {
        if (i == slot) continue;
        ctx[i] = {avcSessions[i].contextOff, avcSessions[i].persistentBytes};
        dpb[i] = {avcSessions[i].dpbOff, avcSessions[i].bankBytes};
    }
    auto &session = avcSessions[slot];
    uint32_t contextOff = 0, dpbOff = 0;
    if (!phx_vcn::avcPoolAlloc(kAvcContextBankOffset, kAvcContextPoolBytes, ctx, kAvcStreamSlotCount,
                               g.persistentBytes, contextOff)) return false;
    if (g.separateDpb && !phx_vcn::avcPoolAlloc(kAvcDpbBankOffset, kAvcDpbPoolBytes, dpb, kAvcStreamSlotCount,
                                                g.dpbBytes, dpbOff)) return false;
    session.contextOff = contextOff;
    session.dpbOff = dpbOff;
    return true;
}

uint32_t PhxBringup::findAvcSlot(uint64_t ownerID) const {
    for (uint32_t i = 0; i < kAvcStreamSlotCount; ++i) {
        const auto &session = avcSessions[i];
        if (session.ownerID == ownerID && (ownerID || !session.persistentBytes)) return i;
    }
    return kAvcStreamSlotCount;
}

bool PhxBringup::avcVramInUse(uint32_t byteOffset) const {
    for (uint32_t i = 0; i < kAvcStreamSlotCount; ++i) {
        const auto &session = avcSessions[i];
        if (session.persistentBytes &&
            (vcnFrameOverlap(byteOffset, sizeof(uint32_t), session.stagingBytes) ||
             (byteOffset >= session.contextOff && byteOffset - session.contextOff < session.persistentBytes) ||
             (session.bankBytes && byteOffset >= session.dpbOff && byteOffset - session.dpbOff < session.bankBytes)))
            return true;
    }
    return false;
}

void PhxBringup::publishAvcSessionCount() {
    uint32_t active = 0;
    bool reserved = false;
    for (const auto &session : avcSessions) {
        if (session.ownerID) ++active;
        reserved |= session.persistentBytes != 0;
    }
    setProperty("PhxAvcActiveSessions", static_cast<unsigned long long>(active), 32);
    if (!active) setProperty("PhxAvcReadbackMode", reserved ? "shutdown-pending" : "idle");
}

bool PhxBringup::avcSessionCommand(uint32_t slot, bool close, uint32_t width, uint32_t height) {
    const uint32_t msg = kDecodeFrameScratchOffset;
    const uint32_t count = buildDecodeSessionMsg(vcnIb, close, width, height, kAvcFirstHandle + slot);
    vcnZeroVram(msg, kAvcStreamAlignment);
    for (uint32_t i = 0; i < count; ++i) vcnWriteVram(msg + 4u*i, vcnIb[i]);
    uint64_t result[8]{};
    return vcnSubmitCodecIb(buildDecodeSessionIb(vcnIb, vcnMc(msg), vcnMc(avcSessions[slot].contextOff)),
                            close ? 3u : 1u, result);
}

bool PhxBringup::closeAvcStreamLocked(uint32_t slot, bool restarting) {
    auto &session = avcSessions[slot];
    if (!session.ownerID) return session.persistentBytes == 0;
    if (!vcnFrameMemoryOk(0, session.bankBytes != 0) || !avcSessionCommand(slot, true, 0, 0)) {
        // The firmware state is no longer known: never let the next IDR reuse this slot.
        session.errorFree = false;
        uint32_t response = 0;
        vcnPowerDown(&response);
        return false;
    }
    session.ownerID = 0;
    session.errorFree = false;   // the next IDR must rebuild rather than reuse a closed session
    session.persistentBytes = session.stagingBytes = session.bankBytes = 0;
    session.parser.reset();
    if (!restarting) {
        releaseAvcTargetMap(session);
        publishAvcSessionCount();
    }
    return true;
}

void PhxBringup::releaseAvcStream(uint64_t client) {
    IOLockLock(vcnLock);
    const uint32_t slot = findAvcSlot(client);
    if (client && slot < kAvcStreamSlotCount && vcnReady && !vcnStopped && !isSleeping)
        closeAvcStreamLocked(slot);
    IOLockUnlock(vcnLock);
}

void PhxBringup::releaseAvcTargetMap(AvcSession &session) {
    if (session.targetMap) {
        session.targetMap->release();
        session.targetMap = nullptr;
    }
    session.targetOffset = session.targetBytes = session.targetMatches = 0;
}

// The reserved Phoenix UMA aperture may be non-snooped by VCN. Before CPU
// reads, invalidate the physical alias in all CPU caches after the VCN fence.
static bool avcHasClflushopt() {
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(0u), "2"(0u));
    if (a < 7u) return false;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(1u), "2"(0u));
    if (!(d & (1u << 19)) || ((b >> 8) & 0xffu) != 8u) return false;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(7u), "2"(0u));
    return (b & (1u << 23)) != 0;
}

bool PhxBringup::readAvcPicture(AvcSession &session, const AvcStreamSequence &s, const AvcStreamGeometry &g,
                              const uint8_t *barTarget, uint8_t *pixels, uint32_t destPitch, uint32_t destChroma) {
    const uint32_t offset = kDecodeFrameScratchOffset + g.targetOffset;
    if (offset != session.targetOffset || g.targetBytes != session.targetBytes) {
        releaseAvcTargetMap(session);
        // Map only the linear target already checked by vcnFrameMemoryOk.
        // Use the APU's system address, as the framebuffer and PSP setup do.
        if (avcMediaEnabled && avcHasClflushopt() && fbPhys &&
            fbPhys == (static_cast<uint64_t>(rreg(PHX_MMMC_VM_FB_OFFSET)) << 24) &&
            rangeFits(g.targetOffset, g.targetBytes, g.stagingBytes) &&
            rangeFits(offset, g.targetBytes, fbSize) &&
            rangeFits(offset, g.targetBytes, fbAperLen) &&
            fbPhys <= UINT64_MAX - offset - g.targetBytes) {
            IODeviceMemory *memory = IODeviceMemory::withRange(fbPhys + offset, g.targetBytes);
            if (memory) {
                session.targetMap = memory->map(kIOMapCopybackCache | kIOMapReadOnly);
                memory->release();
            }
            if (session.targetMap && (!session.targetMap->getVirtualAddress() ||
                                session.targetMap->getLength() != g.targetBytes))
                releaseAvcTargetMap(session);
        }
        session.targetOffset = offset;
        session.targetBytes = g.targetBytes;
        setProperty("PhxAvcReadbackMode", session.targetMap ? "validating-uma" : "bar0");
    }

    const auto *alias = session.targetMap ?
        reinterpret_cast<const uint8_t *>(session.targetMap->getVirtualAddress()) : nullptr;
    const auto flushLine = [](const uint8_t *line, uint32_t bytes) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(line) & ~static_cast<uintptr_t>(63);
        const uintptr_t last = (reinterpret_cast<uintptr_t>(line) + bytes + 63u) & ~static_cast<uintptr_t>(63);
        for (uintptr_t p = first; p < last; p += 64)
            __asm__ __volatile__("clflushopt (%0)" :: "r"(p) : "memory");
    };
    const int flushMode = gAvcFlushMode < 0 ? 0 : (gAvcFlushMode > 2 ? 2 : gAvcFlushMode);
    const uint64_t flushStart = mach_absolute_time();
    if (alias && flushMode != 2) {
        __asm__ __volatile__("mfence" ::: "memory");
        if (flushMode == 0) {
            for (uint32_t i = 0; i < g.targetBytes; i += 64)
                __asm__ __volatile__("clflushopt (%0)" :: "r"(alias + i) : "memory");
        } else {
            // Only the rows the copy reads back. The rest of the target is padding and
            // alignment the decoder writes but the readback never looks at.
            for (uint32_t plane = 0; plane < 2; ++plane) {
                const uint32_t planeOffset = plane * g.lumaBytes;
                const uint32_t rows = s.visibleHeight >> plane;
                for (uint32_t row = 0; row < rows; ++row)
                    flushLine(alias + planeOffset + row * g.pitch, s.visibleWidth);
            }
        }
        __asm__ __volatile__("mfence" ::: "memory");
    }
    const uint64_t flushEnd = mach_absolute_time();

    constexpr uint32_t requiredMatches = 2;
    const bool fast = alias && session.targetMatches >= requiredMatches;
    bool match = alias != nullptr;
    bool sawNonzero = false;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const uint32_t planeOffset = plane * g.lumaBytes;
        const uint32_t rows = s.visibleHeight >> plane;
        const int black = plane ? 128 : 16;
        uint8_t *dst = plane ? pixels + destChroma : pixels;
        for (uint32_t row = 0; row < rows; ++row) {
            const uint32_t rowOffset = planeOffset + row * g.pitch;
            if (flushMode == 2 && alias) {   // interleaved: invalidate the row just before reading it
                flushLine(alias + rowOffset, s.visibleWidth);
                __asm__ __volatile__("mfence" ::: "memory");
            }
            if (fast) {
                memcpy(dst, alias + rowOffset, s.visibleWidth);
            } else {
                readApertureRow(dst, barTarget + rowOffset, s.visibleWidth);
                if (match) {
                    match = memcmp(dst, alias + rowOffset, s.visibleWidth) == 0;
                    for (uint32_t col = 0; !sawNonzero && col < s.visibleWidth; ++col)
                        sawNonzero = dst[col] != 0;
                }
            }
            if (destPitch > s.visibleWidth) memset(dst + s.visibleWidth, black, destPitch - s.visibleWidth);
            dst += destPitch;
        }
    }
    const uint64_t copyEnd = mach_absolute_time();
    uint64_t flushNs = 0, copyNs = 0;
    absolutetime_to_nanoseconds(flushEnd - flushStart, &flushNs);
    absolutetime_to_nanoseconds(copyEnd - flushEnd, &copyNs);
    avcFlushNs = flushNs;
    avcCopyNs = copyNs;
    if (alias && !fast) {
        if (!match) {
            releaseAvcTargetMap(session);
            session.targetOffset = offset;
            session.targetBytes = g.targetBytes;
            setProperty("PhxAvcReadbackMode", "bar0-mismatch");
        } else if (sawNonzero && ++session.targetMatches == requiredMatches) {
            setProperty("PhxAvcReadbackMode", "uma");
        }
    }
    return fast;
}

IOReturn PhxBringup::submitAvcPicture(uint32_t slot, uint64_t client, const AvcStreamSequence &s, const AvcStreamPicture &p,
                                     const AvcStreamGeometry &g, const uint8_t *vcl, uint32_t vclBytes,
                                     uint8_t *pixels, uint32_t destPitch, uint32_t destChroma) {
    const uint32_t serial = avcSerial == UINT32_MAX ? 1u : avcSerial + 1u;
    if (!buildAvcStreamMessage(avcMessage, avcScaling, s, p, kAvcFirstHandle + slot, serial, vclBytes))
        return kIOReturnBadArgument;
    auto &session = avcSessions[slot];
    auto *aperture = reinterpret_cast<uint8_t *>(fbAper->getVirtualAddress());
    auto *bar = aperture + kDecodeFrameScratchOffset;
    if (p.idr) {
        const phx_vcn::AvcSessionKey stored{session.codedWidth, session.codedHeight, session.persistentBytes,
                                            session.stagingBytes, session.bankBytes, session.profileIdc,
                                            session.maxRefs};
        const phx_vcn::AvcSessionKey wanted{s.codedWidth, s.codedHeight, g.persistentBytes, g.stagingBytes,
                                            g.separateDpb ? g.dpbBytes : 0u, s.profileIdc,
                                            static_cast<uint8_t>(s.maxNumRefFrames)};
        // Reusing the session skips the close/create pair and the BAR0 clear of the persistent
        // area. The parser emptied the reference list for this IDR and the frame message carries
        // it, so the firmware sees a complete new reference state (Mesa never recreates its
        // session on an IDR either).
        const bool reuse = session.ownerID == client &&
            phx_vcn::avcSessionMatches(stored, wanted, session.errorFree, gAvcIdrReuse != 0);
        vcnLastIdrReuse = reuse ? 1u : 0u;
        if (reuse) {
            ++session.idrReused;
        } else {
            // One line per rebuild: the steady state reuses, so this only fires on a
            // stream start, a geometry change, a disabled switch or after an error.
            const char *reason = session.ownerID != client ? "new-client"
                               : !session.errorFree ? "unclean"
                               : gAvcIdrReuse == 0 ? "disabled" : "geometry";
            IOLog("PhxBringup: AVC session rebuild slot=%u reason=%s %ux%u\n",
                  slot, reason, s.codedWidth, s.codedHeight);
            if (!closeAvcStreamLocked(slot, true)) return kIOReturnIOError;
            // A half-updated key must never look reusable: until the create succeeds this
            // session has no firmware state at all.
            session.errorFree = false;
            if (!allocAvcMemory(slot, g)) {
                IOLog("PhxBringup: AVC pool full slot=%u need ctx=%u dpb=%u\n", slot, g.persistentBytes,
                      g.separateDpb ? g.dpbBytes : 0u);
                return kIOReturnBusy;
            }
            // The DPB banks sit above the BAR0 aperture and cannot be zeroed from the CPU;
            // the firmware overwrites every reference picture before it is referenced.
            clearAperture(aperture + session.contextOff, g.persistentBytes);
            if (!avcSessionCommand(slot, false, s.codedWidth, s.codedHeight)) {
                uint32_t response = 0;
                vcnPowerDown(&response);
                return kIOReturnIOError;
            }
            session.ownerID = client;
            session.persistentBytes = g.persistentBytes;
            session.stagingBytes = g.stagingBytes;
            session.bankBytes = g.separateDpb ? g.dpbBytes : 0;
            session.codedWidth = s.codedWidth;
            session.codedHeight = s.codedHeight;
            session.profileIdc = s.profileIdc;
            session.maxRefs = static_cast<uint8_t>(s.maxNumRefFrames);
            publishAvcSessionCount();
        }
    }
    uint32_t feedback[kAvcStreamFeedbackDw];
    initAvcStreamFeedback(feedback);
    clearAperture(bar + g.feedbackOffset, kAvcStreamAlignment);
    memcpy(bar + g.feedbackOffset, feedback, sizeof(feedback));
    memcpy(bar + g.messageOffset, avcMessage, sizeof(avcMessage));
    memcpy(bar + g.scalingOffset, avcScaling, sizeof(avcScaling));
    writeAperture(bar + g.bitstreamOffset, vcl, vclBytes);
    clearAperture(bar + g.bitstreamOffset + vclBytes, avcStreamAlign(vclBytes, 128u) - vclBytes);
    // The decoder overwrites every visible row of the target; padding is never read back.
    const auto buffers = avcStreamBuffers(vcnMc(kDecodeFrameScratchOffset), vcnMc(session.contextOff), g,
                                          vcnMc(session.dpbOff));
    uint64_t result[8]{};
    avcSerial = serial;
    avcUploadEnd = mach_absolute_time();
    if (!vcnSubmitCodecIb(buildDecodeFrameIb(vcnIb, buffers), 2, result, !p.idr)) {
        session.errorFree = false;   // VCN is going down: the next IDR has to rebuild
        uint32_t response = 0;
        vcnPowerDown(&response);
        return kIOReturnIOError;
    }
    for (uint32_t i = 0; i < kAvcStreamFeedbackDw; ++i)
        feedback[i] = vcnReadVram(kDecodeFrameScratchOffset + g.feedbackOffset + 4u*i);
    if (!avcStreamFeedbackOK(feedback, serial)) {
        IOLog("PhxBringup: AVC decode feedback serial=%u reported=%u status=%x error=%x\n",
              serial, feedback[3], feedback[4], feedback[6]);
        closeAvcStreamLocked(slot);
        return kIOReturnIOError;
    }
    session.errorFree = true;   // a validated frame is the only proof the session state is consistent
    avcFenceEnd = mach_absolute_time();
    avcUsedUma = readAvcPicture(session, s, g, bar + g.targetOffset, pixels, destPitch, destChroma);
    avcReadEnd = mach_absolute_time();
    return kIOReturnSuccess;
}

IOReturn PhxBringup::decodeAvcStreamLocked(uint64_t client, const uint8_t *au, size_t size,
                                          IOMemoryDescriptor *output, uint32_t pitch, uint32_t chromaOffset) {
    if (vcnStopped || !vcnReady || isSleeping || !mmio || !fbAper) return kIOReturnNotReady;
    uint32_t slot = findAvcSlot(client);
    AvcStreamParser next{};
    if (slot < kAvcStreamSlotCount) next = avcSessions[slot].parser;
    AvcStreamSequence sequence{};
    AvcStreamPicture picture{};
    AvcVclRange range{};
    AvcStreamGeometry geometry{};
    uint32_t slices = 0;
    if (!next.parse(au, size, sequence, picture, slices, &range) || !range.length ||
        !rangeFits(range.offset, range.length, size) || !avcStreamGeometry(sequence, geometry)) {
        // A rejected access unit means the stream lost sync: make the next IDR rebuild
        // the firmware session instead of reusing one whose reference state is unclear.
        if (slot < kAvcStreamSlotCount) avcSessions[slot].errorFree = false;
        return kIOReturnUnsupported;
    }
    if (slot == kAvcStreamSlotCount) {
        if (!picture.idr) return kIOReturnNotReady;
        slot = findAvcSlot(0);
        if (slot == kAvcStreamSlotCount) return kIOReturnBusy;
    }
    auto &session = avcSessions[slot];
    if (!picture.idr && (session.persistentBytes != geometry.persistentBytes ||
                         session.stagingBytes != geometry.stagingBytes ||
                         session.bankBytes != (geometry.separateDpb ? geometry.dpbBytes : 0))) return kIOReturnNotReady;
    // The caller may hand back a whole surface: pitch is the row stride shared by both
    // planes and chromaOffset locates the UV plane. Zero keeps the packed layout used by
    // the diagnostic paths (pitch == visibleWidth, UV straight after the luma plane).
    const uint64_t length = output->getLength();
    uint32_t destPitch = pitch ? pitch : sequence.visibleWidth;
    const uint64_t destChroma = chromaOffset ? chromaOffset :
        static_cast<uint64_t>(destPitch) * sequence.visibleHeight;
    if (!sequence.visibleHeight || (sequence.visibleHeight & 1u) ||
        destPitch < sequence.visibleWidth || (destPitch & 1u) ||
        destChroma < static_cast<uint64_t>(destPitch) * sequence.visibleHeight ||
        destChroma + static_cast<uint64_t>(destPitch) * (sequence.visibleHeight / 2u) > length)
        return kIOReturnBadArgument;
    if (!vcnFrameMemoryOk(geometry.stagingBytes, geometry.separateDpb))
        return kIOReturnNotReady;
    IOReturn status = output->prepare();
    IOMemoryMap *map = nullptr;
    if (status == kIOReturnSuccess) {
        map = output->map();
        if (!map || map->getLength() < length) status = kIOReturnNoMemory;
    }
    if (status == kIOReturnSuccess) {
        auto *pixels = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
        const uint64_t started = mach_absolute_time();
        status = submitAvcPicture(slot, client, sequence, picture, geometry, au + range.offset,
                                  static_cast<uint32_t>(range.length), pixels, destPitch,
                                  static_cast<uint32_t>(destChroma));
        if (status == kIOReturnSuccess) {
            session.parser = next;
            if (avcSerial <= 3 || avcSerial % 12 == 0) {
                uint64_t upload, fence, readback, copyout;
                absolutetime_to_nanoseconds(avcUploadEnd - started, &upload);
                absolutetime_to_nanoseconds(avcFenceEnd - avcUploadEnd, &fence);
                absolutetime_to_nanoseconds(avcReadEnd - avcFenceEnd, &readback);
                absolutetime_to_nanoseconds(mach_absolute_time() - avcReadEnd, &copyout);
                // Keep one complete sample available after the XPC client exits.
                char sample[384];
                snprintf(sample, sizeof(sample),
                         "serial=%u %ux%u idr=%u readback=%s upload_us=%llu fence_us=%llu readback_us=%llu copyout_us=%llu slot=%u pitch=%u"
                         " wait=%u sleeps=%u spin_us=%u hw_ewma_us=%llu idr_reuse=%u flush_us=%llu copy_us=%llu",
                         avcSerial, sequence.visibleWidth, sequence.visibleHeight, unsigned(picture.idr),
                         avcUsedUma ? "uma" : "bar0",
                         upload / 1000, fence / 1000, readback / 1000, copyout / 1000, slot, destPitch,
                         vcnLastWaitHit, vcnLastWaitSleeps, vcnLastWaitSpinUs,
                         static_cast<unsigned long long>(gVcnHwEwmaNs / 1000ull), vcnLastIdrReuse,
                         static_cast<unsigned long long>(avcFlushNs / 1000ull),
                         static_cast<unsigned long long>(avcCopyNs / 1000ull));
                setProperty("PhxAvcLastTiming", sample);
                if (avcSerial % 60 == 0) {   // counters at 1/5 of the sampled rate: setProperty holds vcnLock
                    uint64_t reused = 0;
                    for (uint32_t i = 0; i < kAvcStreamSlotCount; ++i) reused += avcSessions[i].idrReused;
                    char counters[192];
                    snprintf(counters, sizeof(counters),
                             "frames=%u sleeps_total=%llu spin_hits=%llu short_hits=%llu fallback_hits=%llu max_fence_us=%llu idr_reused=%llu",
                             avcSerial, static_cast<unsigned long long>(vcnWaitStats.sleeps),
                             static_cast<unsigned long long>(vcnWaitStats.spinHits),
                             static_cast<unsigned long long>(vcnWaitStats.shortHits),
                             static_cast<unsigned long long>(vcnWaitStats.fallbackHits),
                             static_cast<unsigned long long>(vcnWaitStats.maxNs / 1000ull),
                             static_cast<unsigned long long>(reused));
                    setProperty("PhxAvcCounters", counters);
                }
                if (avcSerial <= 3 || avcSerial % 60 == 0)
                    IOLog("PhxBringup: AVC %s\n", sample);
            }
        } else closeAvcStreamLocked(slot); // Firmware already advanced this session's reference.
    }
    if (map) map->release();
    const IOReturn completed = output->complete();
    if (status == kIOReturnSuccess && completed != kIOReturnSuccess) {
        status = completed;
        closeAvcStreamLocked(slot);
    }
    return status;
}

IOReturn PhxBringup::decodeAvcStream(uint64_t client, const uint8_t *au, size_t size, IOMemoryDescriptor *output,
                                     uint32_t pitch, uint32_t chromaOffset) {
    if (!client || !vcnLock || !au || !size || size > kAvcStreamMaxBitstreamBytes || !output)
        return kIOReturnBadArgument;
    IOLockLock(vcnLock);
    IOReturn status = decodeAvcStreamLocked(client, au, size, output, pitch, chromaOffset);
    IOLockUnlock(vcnLock);
    return status;
}

IOReturn PhxBringup::vcnCodecFrame(bool encode, uint8_t *output, uint32_t capacity, uint64_t out[8],
                                   const uint8_t *packedNv12, const EncodeGeometry &g, const uint8_t *referenceIDR) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!vcnLock)
        return kIOReturnNotReady;
    if (!output || capacity < (encode ? g.bitstreamBytes : kDecodeTargetBytes))
        return kIOReturnBadArgument;
    IOLockLock(vcnLock);
    if (vcnStopped || !mmio || !fbAper) {
        IOLockUnlock(vcnLock);
        return kIOReturnOffline;
    }
    if (!vcnReady || isSleeping) {
        out[0] = PHX_VCN_NOT_READY;
        IOLockUnlock(vcnLock);
        return kIOReturnSuccess;
    }
    if (!vcnFrameMemoryOk(encode ? g.scratchBytes : kDecodeFrameRequiredBytes)) {
        out[0] = PHX_VCN_FRAME_REGION;
        IOLockUnlock(vcnLock);
        return kIOReturnSuccess;
    }

    auto *bar = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    const uint64_t contextMc = vcnMc(kBoOffset + kCodecDataOff);
    const uint32_t frameBase = g.scratchBytes ? kDecodeFrameScratchOffset : kBoOffset;
    const uint64_t codecMc = vcnMc(frameBase);
    const auto buffers = makeDecodeFrameBuffers(vcnMc(kDecodeFrameScratchOffset));
    const auto copyVector = [bar](uint32_t offset, const uint8_t *data, uint32_t size) {
        for (uint32_t i = 0; i < size; ++i)
            bar[kDecodeFrameScratchOffset + offset + i] = data[i];
    };
    if (encode) {
        vcnZeroVram(kBoOffset + kCodecDataOff, kEncodeContextBytes);
        vcnZeroVram(frameBase + g.cpbOff, g.inputOff - g.cpbOff);
        // Avoid stale feedback or output being returned if the firmware fails.
        vcnZeroVram(frameBase + g.bitstreamOff, g.feedbackOff + kEncodeFrameFeedbackBytes - g.bitstreamOff);
        volatile uint8_t *input = bar + frameBase + g.inputOff;
        if (packedNv12)
            copyPackedNv12ToEncodeFrame(input, g.lumaBytes * 3u / 2u, packedNv12, g.width * g.height * 3u / 2u, g);
        else
            initializeEncodeFrame(input, kEncodeFrameInputBytes);
    } else {
        vcnZeroVram(kDecodeFrameScratchOffset, kDecodeFrameRequiredBytes);
    }

    uint64_t frameFailure[8] = {};
    for (uint32_t phase = 1; phase <= 3; ++phase) {
        uint32_t ibDw;
        if (encode && phase == 2) {
            ibDw = buildEncodeFrameIb(vcnIb, contextMc, codecMc + g.cpbOff,
                                      codecMc + g.inputOff, codecMc + g.bitstreamOff,
                                      codecMc + g.feedbackOff, phase, g);
        } else if (encode) {
            ibDw = buildEncodeFrameSessionIb(vcnIb, contextMc, phase, phase == 3, g);
        } else if (phase == 2) {
            copyVector(kDecodeFrameMessageOffset, kDecodeH264Message, sizeof(kDecodeH264Message));
            copyVector(kDecodeFrameMessageOffset + sizeof(kDecodeH264Message), kDecodeH264Parameters,
                       sizeof(kDecodeH264Parameters));
            copyVector(kDecodeFrameFeedbackOffset, kDecodeH264Feedback, sizeof(kDecodeH264Feedback));
            copyVector(kDecodeFrameScalingTableOffset, kDecodeH264ScalingTable, sizeof(kDecodeH264ScalingTable));
            // The frame scratch, including the aligned bitstream's final 12 bytes, was cleared above.
            copyVector(kDecodeFrameBitstreamOffset, referenceIDR ? referenceIDR : kDecodeH264Bitstream,
                       PHX_VIDEO_DECODE_INPUT_BYTES);
            ibDw = buildDecodeFrameIb(vcnIb, buffers);
        } else {
            const uint32_t msgDw = buildDecodeFrameSessionMsg(vcnIb, phase == 3);
            vcnZeroVram(kDecodeFrameScratchOffset + kDecodeFrameMessageOffset, 4096);
            copyVector(kDecodeFrameMessageOffset, reinterpret_cast<const uint8_t *>(vcnIb), msgDw * 4u);
            ibDw = buildDecodeSessionIb(vcnIb, buffers.message, buffers.sessionContext);
        }
        if (!vcnSubmitCodecIb(ibDw, phase, out)) {
            uint32_t response = 0;
            vcnPowerDown(&response);
            break;
        }
        if (phase != 2)
            continue;
        if (encode) {
            const auto *feedback = bar + frameBase + g.feedbackOff;
            const auto *stream = bar + frameBase + g.bitstreamOff;
            const uint32_t reported = readLe32(feedback + 6u * sizeof(uint32_t));
            out[7] = reported;
            uint32_t bytes = 0;
            if (reported && reported <= g.bitstreamBytes) {
                // Only read the aperture once. Validate in CPU memory before
                // publishing the length to the requesting user client.
                memcpy(output, const_cast<const uint8_t *>(stream), reported);
                if (encodedH264IdrOk(feedback, kEncodeFrameFeedbackBytes, output, reported, &bytes))
                    out[6] = bytes;
            }
        } else {
            const auto *pixels = bar + kDecodeFrameScratchOffset + kDecodeFrameTargetOffset;
            uint32_t sum = 0;
            for (uint32_t i = 0; i < kDecodeTargetBytes; ++i) {
                const uint8_t pixel = pixels[i];
                output[i] = pixel;
                sum += pixel;
            }
            out[7] = sum;
            if (sum == kDecodeOutputByteSum)
                out[6] = kDecodeTargetBytes;
        }
        if (!out[6]) {
            memcpy(frameFailure, out, sizeof(frameFailure));
            frameFailure[0] = PHX_VCN_BAD_FRAME;
        }
    }
    if (out[0] == PHX_VCN_OK && frameFailure[0])
        memcpy(out, frameFailure, sizeof(frameFailure));
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vcnShutdown(uint64_t out[8]) {
    bzero(out, sizeof(uint64_t) * 8u);
    if (!mmio || !fbAper || !vcnLock)
        return kIOReturnNotReady;
    IOLockLock(vcnLock);
    if (isSleeping || !vcnNeedsPowerDown) {
        out[0] = PHX_VCN_NOT_READY;
    } else {
        uint32_t response = 0;
        out[0] = vcnPowerDown(&response) ? PHX_VCN_OK : PHX_VCN_POWERDOWN;
        out[1] = response;
    }
    vcnRecord(out);
    IOLockUnlock(vcnLock);
    return kIOReturnSuccess;
}

bool PhxBringup::claimSession() { return OSCompareAndSwap(0, 1, &sessionOpen); }

void PhxBringup::releaseSession() { OSCompareAndSwap(1, 0, &sessionOpen); }

IOReturn PhxBringup::readRegs(uint32_t first, uint32_t count, uint32_t *out) {
    if (!mmio)
        return kIOReturnNotReady;
    uint64_t len = mmio->getLength();
    if ((static_cast<uint64_t>(first) + count) * 4 > len)
        return kIOReturnBadArgument;
    volatile uint32_t *r = reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress());
    for (uint32_t i = 0; i < count; i++)
        out[i] = r[first + i];
    return kIOReturnSuccess;
}

IOReturn PhxBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    // the sysctls go up with the first client, from user space, rather than in start(): the bootloader injects this
    // kext into the boot collection, and it starts long before the rest of BSD
    if (!gSysctlUp && OSCompareAndSwap(0, 1, &gSysctlUp)) {
        sysctl_register_oid(&sysctl__debug_phxbos_flush);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_register_oid(&sysctl__debug_phx_hang_ms);
        sysctl_register_oid(&sysctl__debug_phx_hang_kills);
        sysctl_register_oid(&sysctl__debug_phx_vcn_wait);
        sysctl_register_oid(&sysctl__debug_phx_vcn_spin_max_us);
        sysctl_register_oid(&sysctl__debug_phx_vcn_slice_us);
        sysctl_register_oid(&sysctl__debug_phx_vcn_hw_ewma_ns);
        sysctl_register_oid(&sysctl__debug_phx_avc_idr_reuse);
        sysctl_register_oid(&sysctl__debug_phx_avc_flush_mode);
    }
    if (type != PHXN_UC_TYPE)
        return kIOReturnBadArgument;
    IOAccelPhxNativeClient *client = OSTypeAlloc(IOAccelPhxNativeClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- user client ----------------------------------------------------------------------------------------

bool IOAccelPhxNativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    privileged = clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task = owningTask;
    lock = IOLockAlloc();
    bos = static_cast<Bo *>(IOMallocZero(sizeof(Bo) * PHXN_MAX_BOS));
    maps = static_cast<Map *>(IOMallocZero(sizeof(Map) * kMaxMaps));
    return lock && bos && maps;
}

bool IOAccelPhxNativeClient::start(IOService *provider) {
    owner = OSDynamicCast(PhxBringup, provider);
    if (!owner)
        return false;
    // E9: several processes at once, each in its own GPUVM context (vmidAlloc); the first one still marks the session
    if (owner->claimSession())
        sessionClaimed = true;
    return IOUserClient::start(provider);
}

void IOAccelPhxNativeClient::freeAll() {
    if (!bos)
        return;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++)
        if (bos[h].md)
            boRelease(bos[h]);
}

IOReturn IOAccelPhxNativeClient::clientClose() {
    if (lock)
        IOLockLock(lock);
    avcClientClosed = true;
    releaseAvcProvider();
    freeAll();
    if (lock)
        IOLockUnlock(lock);
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (sessionClaimed && owner) {
        owner->releaseSession();
        sessionClaimed = false;
    }
    terminate();
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::free() {
    releaseAvcProvider();
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    freeAll();
    if (sessionClaimed && owner)
        owner->releaseSession();
    if (bos)
        IOFree(bos, sizeof(Bo) * PHXN_MAX_BOS);
    if (maps)
        IOFree(maps, sizeof(Map) * kMaxMaps);
    if (lock)
        IOLockFree(lock);
    IOUserClient::free();
}

IOAccelPhxNativeClient::Bo *IOAccelPhxNativeClient::findBo(uint32_t h) {
    if (h == 0 || h >= PHXN_MAX_BOS || !bos[h].md)
        return nullptr;
    return &bos[h];
}

IOReturn IOAccelPhxNativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOLockLock(lock);
    Bo *b = findBo(type);
    if (!b) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    b->md->retain();
    *memory = b->md;
    *options = 0;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

// Exact shape check, as in the contract: scalar counts and struct sizes must match.
static bool shape(const IOExternalMethodArguments *a, uint32_t sin, uint32_t sout, uint32_t stin, uint32_t stout) {
    return a->scalarInputCount == sin && a->scalarOutputCount == sout && a->structureInputSize == stin &&
           a->structureOutputSize == stout && !a->structureInputDescriptor && !a->structureOutputDescriptor;
}

IOReturn IOAccelPhxNativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector == PHXN_SEL_WAITSEQ)
        return doWaitSeq(a);
    if (selector == PHXN_SEL_TOPOLOGY)
        return doTopology(a);
    if (selector == PHXN_SEL_SURF_GPU_MARK)   // no client state: a ready mark must not wait behind a submission
        return doSurfGpuMark(a);

    IOLockLock(lock);
    IOReturn r;
    if (selector != PHXN_SEL_HELLO && !hello) {
        r = kIOReturnNotReady;
    } else {
        switch (selector) {
        case PHXN_SEL_HELLO:     r = doHello(a); break;
        case PHXN_SEL_QUERYINFO: r = doQueryInfo(a); break;
        case PHXN_SEL_READREGS:  r = doReadRegs(a); break;
        case PHXN_SEL_BOCREATE:  r = doBoCreate(a); break;
        case PHXN_SEL_BOFREE:    r = doBoFree(a); break;
        case PHXN_SEL_BOIMPORTHOST: r = doBoImportHost(a); break;
        case PHXN_SEL_GEMVA:     r = doGemVa(a); break;
        case PHXN_SEL_CTX:       r = doCtx(a); break;
        case PHXN_SEL_SUBMIT:    r = doSubmit(a); break;
        case PHXN_SEL_VCN_ENCODE_H264: r = doEncodeH264(a, false); break;
        case PHXN_SEL_VCN_ENCODE_H264_SIZED: r = doEncodeH264(a, true); break;
        case PHXN_SEL_VCN_ENCODE_READY:
            if (!shape(a, 0, 1, 0, 0) || a->asyncWakePort) {
                r = kIOReturnBadArgument;
            } else {
                a->scalarOutput[0] = owner->vcnEncoderReady();
                r = kIOReturnSuccess;
            }
            break;
        case PHXN_SEL_DBG_SMU:   r = doDbgSmu(a); break;
        case PHXN_SEL_DBG_PSP_RING: r = doDbgPspRing(a); break;
        case PHXN_SEL_DBG_VRAM_READ: r = doDbgVramRead(a); break;
        case PHXN_SEL_DBG_PSP_SUBMIT: r = doDbgPspSubmit(a); break;
        case PHXN_SEL_DBG_FW_STAGE: r = doDbgFwStage(a); break;
        case PHXN_SEL_DBG_PSP_LOADIPFW: r = doDbgLoadIpFw(a); break;
        case PHXN_SEL_DBG_PSP_CTL: r = doDbgPspCtl(a); break;
        case PHXN_SEL_DBG_PSP_RAW: r = doDbgPspRaw(a); break;
        case PHXN_SEL_DBG_WRITE_REG: r = doDbgWriteReg(a); break;
        case PHXN_SEL_DBG_VRAM_WRITE: r = doDbgVramWrite(a); break;
        case PHXN_SEL_DBG_DOORBELL: r = doDbgDoorbell(a); break;
        case PHXN_SEL_DBG_SYSMEM: r = doDbgSysMem(a); break;
        case PHXN_SEL_DBG_ADOPTQ: r = doDbgAdoptQueue(a); break;
        case PHXN_SEL_DBG_CLAIM: r = doDbgClaim(a); break;
        case PHXN_SEL_DBG_VCN:   r = doDbgVcn(a); break;
        case PHXN_SEL_DBG_AVC_DECODE: r = doDbgAvcDecode(a); break;
        default:                 r = kIOReturnBadArgument; break;
        }
    }
    IOLockUnlock(lock);
    return r;
}

IOReturn IOAccelPhxNativeClient::doHello(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    if (a->scalarInput[1] != 0 && a->scalarInput[1] != PHXN_HELLO_F_MINOR)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] != PHXN_ABI_VERSION)
        return kIOReturnUnsupported;
    hello = true;
    a->scalarOutput[0] = PHXN_ABI_VERSION | (a->scalarInput[1] ? static_cast<uint64_t>(PHXN_ABI_MINOR) << 16 : 0);
    a->scalarOutput[1] = PHX_KEXT_BUILD;
    a->scalarOutput[2] = PHXN_MAX_IBS;
    a->scalarOutput[3] = PHXN_VMID;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doQueryInfo(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phxn_info)))
        return kIOReturnBadArgument;
    phxn_info inf;
    bzero(&inf, sizeof(inf));
    inf.abi_version = PHXN_ABI_VERSION;
    inf.kext_build = PHX_KEXT_BUILD;
    inf.reserved[2] = PHXN_ABI_MINOR;
    inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT;
    inf.vmid = PHXN_VMID;
    inf.gb_addr_config = kGbAddrConfig;
    inf.gc_version = kGcVersion;
    inf.ring_size_dw = 4096;
    inf.max_ibs = PHXN_MAX_IBS;
    inf.vram_vis_total = kVisTotal;
    inf.vram_vis_free = kVisTotal - visUsed;
    inf.vram_hi_total = kHiTotal;
    inf.vram_hi_free = kHiTotal - hiUsed;
    inf.gtt_cap = kGttCap;
    inf.gtt_used = gttUsed;
    inf.gtt_max_bo = kGttMaxBo;
    inf.seq_emitted = emitted;
    inf.seq_retired = owner->gpuReady() ? owner->gpuRetired() : emitted;
    inf.va_low_first = 0x10000;
    inf.va_low_last = 0x7FFFFFFFFFFFull;
    inf.va_high_first = 0xFFFF800000000000ull;
    inf.va_high_last = 0xFFFFFFFFFFFFFFFFull;
    inf.max_bos = PHXN_MAX_BOS;
    inf.fence_slots = PHXN_FENCE_SLOTS;
    bcopy(&inf, a->structureOutput, sizeof(inf));
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doTopology(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phx_topology)))
        return kIOReturnBadArgument;
    const phx_topology *p = owner->capturedTopology();
    if (!p)
        return kIOReturnUnsupported;
    bcopy(p, a->structureOutput, sizeof(*p));
    return kIOReturnSuccess;
}

static constexpr uint64_t kGoldenTscUpper = 0x16a2d;
IOReturn IOAccelPhxNativeClient::doReadRegs(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 3 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t count = a->scalarInput[1];
    if (count < 1 || count > 16 || a->scalarInput[2] != 0xFFFFFFFFull || a->structureOutputSize != count * 4 ||
        a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    // SMUIO's GOLDEN_TSC (upper 0x16a2d, lower 0x16a2e on Phoenix, SMUIO 13.0.7 base 0x16a00) is the 100 MHz counter the
    // CP stamps command buffers with: every client may read it (Mesa's AMDGPU_INFO_TIMESTAMP, which calibrates GPU
    // times against the host's; without it Metal's GPUStartTime/GPUEndTime were 0 in every app).
    const uint64_t first = a->scalarInput[0];
    const bool tsc = first >= kGoldenTscUpper && first + count <= kGoldenTscUpper + 2;
    if (!privileged && !tsc)
        return kIOReturnNotPrivileged;
    return owner->readRegs(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doBoCreate(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 4, sizeof(phxn_gem_create_in), 0))
        return kIOReturnBadArgument;
    const phxn_gem_create_in *in = static_cast<const phxn_gem_create_in *>(a->structureInput);
    uint64_t size = in->bo_size;
    if (size == 0)
        return kIOReturnBadArgument;
    size = (size + 4095) & ~4095ull;
    uint64_t align = in->alignment;
    if (align && ((align & (align - 1)) || align > (2ull << 20)))
        return kIOReturnBadArgument;
    if (align < 4096)
        align = 4096;
    const uint64_t d = in->domains, f = in->domain_flags;
    if (d == 0 || (d & ~static_cast<uint64_t>(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM)))
        return kIOReturnUnsupported;
    if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE))
        return kIOReturnUnsupported;

    uint32_t domain, bits;
    uint64_t *pool;
    if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
        if (visUsed + size <= kVisTotal) {
            domain = 4;
            bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED;
            pool = &visUsed;
        } else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
                   hiUsed + size <= kHiTotal) {
            domain = 4;
            bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED;
            pool = &hiUsed;
        } else {
            return kIOReturnNoMemory;
        }
    } else {
        if (size > kGttMaxBo || gttUsed + size > kGttCap)
            return kIOReturnNoMemory;
        domain = 2;
        bits = PHXN_PLACED_ZEROED;
        pool = &gttUsed;
    }

    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;

    // Page alignment only: the GPU maps BOs page by page, and the client picks the VA (GEMVA) with its own alignment.
    // Handed to IOBufferMemoryDescriptor, RADV's 64 KiB..2 MiB alignment made every allocation PHYSICALLY CONTIGUOUS
    // (XNU allocates an alignment above the page size with kmem_alloc_contig): once memory was fragmented, every
    // buffer of 8 MiB or more failed after a 400 ms search, and each Metal app spent ~0.8 s at launch (2026-10-21).
    (void)align;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;
    bzero(md->getBytesNoCopy(), size);
    *pool += size;
    bos[h] = Bo{md, size, domain, bits, static_cast<uint8_t *>(md->getBytesNoCopy()), false};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = domain;
    a->scalarOutput[3] = bits;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doBoFree(IOExternalMethodArguments *a) {
    if (!shape(a, 1, 0, 0, 0))
        return kIOReturnBadArgument;
    uint32_t h = static_cast<uint32_t>(a->scalarInput[0]);
    Bo *b = findBo(h);
    if (!b)
        return kIOReturnNotFound;
    for (int i = 0; i < kMaxMaps; i++)
        if (maps[i].used && maps[i].handle == h)
            maps[i].used = false;
    if (b->host)
        hostUsed -= b->size;
    else if (b->domain == 2)
        gttUsed -= b->size;
    else if (b->bits & PHXN_PLACED_HI_POOL)
        hiUsed -= b->size;
    else
        visUsed -= b->size;
    boRelease(*b);
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::boRelease(Bo &b) {
    if (b.host)
        b.md->complete();
    b.md->release();
    b = Bo{};
}

// PHXN_SEL_BOIMPORTHOST (ABI 1.9, VK_EXT_external_memory_host): a BO over the caller's own pages, wired for as long as
// it lives, GPU-mapped later through GemVa like any other. The kext reported ABI 1.12 without it, so every import
// RADV made (no-copy buffers, IOSurface textures over their surfaces' pages) failed and fell back to copies
// (2026-10-20). in: [0] host VA [1] size (both page-aligned) [2] flags 0 [3] GPU VA 0; out: [0] handle [1] size
// [2] GPU VA 0 [3] placement bits.
IOReturn IOAccelPhxNativeClient::doBoImportHost(IOExternalMethodArguments *a) {
    if (!shape(a, 4, 4, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t va = a->scalarInput[0], size = a->scalarInput[1];
    if (!va || !size || (va & 4095) || (size & 4095) || a->scalarInput[2] || a->scalarInput[3] || !task)
        return kIOReturnBadArgument;
    if (size > kHostMaxBo || hostUsed + size > kHostCap)
        return kIOReturnNoMemory;
    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;
    const IOReturn r = md->prepare();
    if (r != kIOReturnSuccess) {
        md->release();
        return r;
    }
    hostUsed += size;
    bos[h] = Bo{md, size, 2, 0, nullptr, true};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = 0;
    a->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doGemVa(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_gem_va), 0))
        return kIOReturnBadArgument;
    const phxn_gem_va *v = static_cast<const phxn_gem_va *>(a->structureInput);
    if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles ||
        v->input_fence_syncobj_handles)
        return kIOReturnBadArgument;
    if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT))
        return kIOReturnUnsupported;
    if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE)
        return kIOReturnUnsupported;
    if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP)
        return kIOReturnBadArgument;
    Bo *b = findBo(v->handle);
    if (!b)
        return kIOReturnNotFound;
    if (!vaCanonical(v->va_address))
        return kIOReturnBadArgument;
    const uint64_t va = v->va_address & kVaMask;
    if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0)
        return kIOReturnBadArgument;
    if (va < 0x10000 || va + v->map_size > (1ull << 48))
        return kIOReturnBadArgument;
    if (v->offset_in_bo + v->map_size > b->size)
        return kIOReturnBadArgument;

    if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
        int freei = -1;
        for (int i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) {
                if (freei < 0)
                    freei = i;
                continue;
            }
            if (va < maps[i].va + maps[i].size && maps[i].va < va + v->map_size)
                return kIOReturnBadArgument;
        }
        if (freei < 0)
            return kIOReturnNoMemory;
        maps[freei] = Map{true, v->handle, va, v->map_size, v->offset_in_bo, v->flags};
        if (owner->gpuReady()) {
            const IOReturn r = ensureVmid() ? owner->vmMap(vmid, va, b->md, v->offset_in_bo, v->map_size) : kIOReturnNoResources;
            if (r != kIOReturnSuccess) {
                maps[freei].used = false;
                return r;
            }
        }
        return kIOReturnSuccess;
    }
    for (int i = 0; i < kMaxMaps; i++) {
        if (maps[i].used && maps[i].handle == v->handle && maps[i].va == va && maps[i].size == v->map_size) {
            maps[i].used = false;
            if (vmid)
                owner->vmUnmap(vmid, va, v->map_size);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IOAccelPhxNativeClient::doCtx(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_ctx), sizeof(phxn_ctx)))
        return kIOReturnBadArgument;
    phxn_ctx in, out;
    bcopy(a->structureInput, &in, sizeof(in));
    bzero(&out, sizeof(out));
    if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
        uint32_t id = 1;
        while (id <= PHXN_MAX_CTX && ctx[id])
            id++;
        if (id > PHXN_MAX_CTX)
            return kIOReturnNoResources;
        ctx[id] = true;
        out.op = id;
    } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
        if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !ctx[in.ctx_id])
            return kIOReturnNotFound;
        ctx[in.ctx_id] = false;
    } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) {
        return kIOReturnBadArgument;
    }
    bcopy(&out, a->structureOutput, sizeof(out));
    return kIOReturnSuccess;
}

bool IOAccelPhxNativeClient::ibCovered(uint64_t vaCanon, uint64_t bytes) {
    uint64_t cur = vaCanon & kVaMask;
    const uint64_t end = cur + bytes;
    while (cur < end) {
        bool ok = false;
        for (int i = 0; i < kMaxMaps; i++) {
            const Map &mp = maps[i];
            if (mp.used && (mp.flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= mp.va && cur < mp.va + mp.size) {
                cur = mp.va + mp.size < end ? mp.va + mp.size : end;
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

IOReturn IOAccelPhxNativeClient::doSubmit(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 0 || a->scalarOutputCount != 1 || a->structureOutputSize != 0 ||
        a->structureInputDescriptor || a->structureInputSize < 64 ||
        a->structureInputSize > 32 + 32 * PHXN_MAX_IBS)
        return kIOReturnBadArgument;
    const uint8_t *in = static_cast<const uint8_t *>(a->structureInput);
    phxn_cs_in h;
    bcopy(in, &h, sizeof(h));
    if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS ||
        a->structureInputSize != sizeof(h) + h.num_ibs * sizeof(phxn_cs_ib) || h.reserved != 0 ||
        (h.flags & ~PHXN_CS_HAS_FENCE))
        return kIOReturnBadArgument;
    if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !ctx[h.ctx_id])
        return kIOReturnNotFound;

    for (uint32_t i = 0; i < h.num_ibs; i++) {
        phxn_cs_ib ib;
        bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
        if (ib._pad || (ib.ip_type != PHX_AMDGPU_HW_IP_GFX && ib.ip_type != PHX_AMDGPU_HW_IP_COMPUTE) || ib.ip_instance != 0 || ib.ring != 0)
            return kIOReturnBadArgument;
        if (ib.ib_bytes == 0 || (ib.ib_bytes & 3) || ib.ib_bytes > 0xFFFFFu * 4)
            return kIOReturnBadArgument;
        if (!vaCanonical(ib.va_start) || (ib.va_start & 3))
            return kIOReturnBadArgument;
        if (ib.flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE |
                        PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
            return kIOReturnUnsupported;
        if (ib.flags & ~static_cast<uint32_t>(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT |
                                              PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
            return kIOReturnBadArgument;
        if (!ibCovered(ib.va_start, ib.ib_bytes))
            return kIOReturnBadArgument;
    }

    Bo *fb = nullptr;
    if (h.flags & PHXN_CS_HAS_FENCE) {
        fb = findBo(h.fence_handle);
        if (!fb)
            return kIOReturnNotFound;
        if (!fb->kva || (h.fence_offset & 7) || static_cast<uint64_t>(h.fence_offset) + 8 > fb->size)
            return kIOReturnBadArgument;   // the kernel writes the fence: not into a host import
    } else if (h.fence_handle || h.fence_offset) {
        return kIOReturnBadArgument;
    }

    if (owner->gpuReady()) {
        // E8: real execution on the adopted compute queue, IBs run in VMID 8
        uint64_t va[PHXN_MAX_IBS];
        uint32_t dw[PHXN_MAX_IBS];
        for (uint32_t i = 0; i < h.num_ibs; i++) {
            phxn_cs_ib ib;
            bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
            va[i] = ib.va_start & kVaMask;
            dw[i] = ib.ib_bytes / 4;
        }
        // log the IBs RADV sends (first 96 dwords each) for bring-up: a client's first submissions only, as every
        // WindowServer frame dumped its IBs and the log held nothing else (2026-10-20)
        if (owner->debugEnabled() && emitted < 4) {
            for (uint32_t i = 0; i < h.num_ibs; i++) {
                for (int m = 0; m < kMaxMaps; m++) {
                    if (!maps[m].used || va[i] < maps[m].va || va[i] >= maps[m].va + maps[m].size)
                        continue;
                    Bo *bo = findBo(maps[m].handle);
                    if (!bo || !bo->kva)
                        break;
                    const uint32_t *d = reinterpret_cast<const uint32_t *>(bo->kva + maps[m].off + (va[i] - maps[m].va));
                    const uint32_t cnt = dw[i] < 96 ? dw[i] : 96;
                    for (uint32_t j = 0; j < cnt; j += 8)
                        IOLog("PhxIB %u.%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(emitted + 1), i, j,
                              d[j], j + 1 < cnt ? d[j + 1] : 0, j + 2 < cnt ? d[j + 2] : 0, j + 3 < cnt ? d[j + 3] : 0,
                              j + 4 < cnt ? d[j + 4] : 0, j + 5 < cnt ? d[j + 5] : 0, j + 6 < cnt ? d[j + 6] : 0,
                              j + 7 < cnt ? d[j + 7] : 0);
                    break;
                }
            }
        }
        flushBos();
        uint64_t seq = 0;
        phxn_cs_ib ib0;
        bcopy(in + sizeof(h), &ib0, sizeof(ib0));
        if (!ensureVmid())
            return kIOReturnNoResources;
        const IOReturn r = owner->gpuSubmit(vmid, ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, va, dw, h.num_ibs, &seq);
        if (r != kIOReturnSuccess)
            return r;
        lastSeq = seq;
        emitted = seq;   // externalMethod already holds `lock`
        if (fb) {
            pendFenceHandle = h.fence_handle;
            pendFenceOffset = h.fence_offset;
            pendFenceSeq = seq;
        }
        a->scalarOutput[0] = seq;
        return kIOReturnSuccess;
    }
    // E1: no GPU behind the call; the submission retires at once, as in the mock.
    const uint64_t seq = ++emitted;
    if (fb)
        bcopy(&seq, fb->kva + h.fence_offset, sizeof(seq));
    a->scalarOutput[0] = seq;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doDbgSmu(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 2, 0, 0) || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    switch (a->scalarInput[0]) {
    case PHX_PPSMC_TestMessage:
    case PHX_PPSMC_GetPmfwVersion:
    case PHX_PPSMC_GetDriverIfVersion:
    case PHX_PPSMC_GetEnabledSmuFeatures:
    case PHX_PPSMC_AllowGfxOff:
    case PHX_PPSMC_DisallowGfxOff:
    case PHX_PPSMC_EnableGfxImu:
    case PHX_PPSMC_GetGfxclkFrequency:
    case PHX_PPSMC_GetFclkFrequency:
    case PHX_PPSMC_SetHardMinGfxClk:
    case PHX_PPSMC_SetSoftMaxGfxClk:
    case PHX_PPSMC_SetHardMinFclkByFreq:
    case PHX_PPSMC_SetSoftMaxFclkByFreq:
    case PHX_PPSMC_SetHardMinSocclkByFreq:
    case PHX_PPSMC_SetSoftMaxSocclkByFreq:
        break;
    default:
        return kIOReturnNotPermitted;
    }
    uint32_t resp = 0, after = 0;
    IOReturn r = owner->smuMessage(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                   &resp, &after);
    a->scalarOutput[0] = resp;
    a->scalarOutput[1] = after;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRing(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRing(static_cast<uint32_t>(a->scalarInput[0]), a->scalarInput[1], out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramRead(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t off = a->scalarInput[0], count = a->scalarInput[1];
    if (count < 1 || count > 256 || off > 0xFFFFFFFFull || a->structureOutputSize != count * 4)
        return kIOReturnBadArgument;
    return owner->vramRead(static_cast<uint32_t>(off), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doDbgPspSubmit(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspSubmit(static_cast<uint32_t>(a->scalarInput[0]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgFwStage(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarOutputCount != 0 || a->structureOutputSize != 0)
        return kIOReturnBadArgument;
    if (a->structureInputDescriptor || !a->structureInput || a->structureInputSize == 0 ||
        a->structureInputSize > 4096 || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->fwStage(static_cast<uint32_t>(a->scalarInput[0]), a->structureInput, a->structureInputSize);
}

IOReturn IOAccelPhxNativeClient::doDbgLoadIpFw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull ||
        a->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->loadIpFw(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                 static_cast<uint32_t>(a->scalarInput[2]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspCtl(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspCtl(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRaw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 7, 4, 0, 0))
        return kIOReturnBadArgument;
    uint32_t w[6];
    for (int i = 0; i < 6; i++)
        w[i] = static_cast<uint32_t>(a->scalarInput[i + 1]);
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRaw(static_cast<uint32_t>(a->scalarInput[0]), w, out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgWriteReg(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 1, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint32_t rb = 0;
    IOReturn r = owner->writeReg(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), &rb);
    a->scalarOutput[0] = rb;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramWrite(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 0, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->vramWrite(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]));
}

IOReturn IOAccelPhxNativeClient::doDbgDoorbell(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->doorbellWrite(a->scalarInput[0], a->scalarInput[1] | (a->scalarInput[2] << 32));
}

IOReturn IOAccelPhxNativeClient::doDbgSysMem(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 2, 0, 0))
        return kIOReturnBadArgument;
    return owner->sysMem(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], &a->scalarOutput[0], &a->scalarOutput[1]);
}

IOReturn IOAccelPhxNativeClient::doDbgAdoptQueue(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->adoptQueue(a->scalarInput);
}

IOReturn IOAccelPhxNativeClient::doDbgClaim(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 0, 0, 0) || a->scalarInput[0] != 1)
        return kIOReturnBadArgument;
    return owner->claimAccel();
}


void IOAccelPhxNativeClient::releaseAvcProvider() {
    if (avcProvider) {
        avcProvider->releaseAvcStream(getRegistryEntryID());
        avcProvider->release();
        avcProvider = nullptr;
    }
}

IOReturn IOAccelPhxNativeClient::doDbgAvcDecode(IOExternalMethodArguments *a) {
    IOMemoryDescriptor *input = a->structureInputDescriptor;
    IOMemoryDescriptor *output = a->structureOutputDescriptor;
    const uint32_t capacity = a->structureOutputDescriptorSize;
    const uint32_t inlineCapacity = a->structureOutputSize;
    a->structureOutputSize = a->structureOutputDescriptorSize = 0;
    if (avcClientClosed || !owner || !owner->debugEnabled()) return kIOReturnUnsupported;
    if (!privileged || current_task() != task) return kIOReturnNotPrivileged;
    const IOByteCount size = input ? input->getLength() : a->structureInputSize;
    if (a->asyncWakePort || a->asyncReferenceCount || a->scalarInputCount || a->scalarOutputCount ||
        a->structureVariableOutputData || inlineCapacity || !output || capacity != output->getLength() ||
        !size || size > kAvcStreamMaxBitstreamBytes ||
        (input ? a->structureInputSize != 0 : !a->structureInput)) return kIOReturnBadArgument;
    IOReturn status = kIOReturnSuccess;
    const uint8_t *au = static_cast<const uint8_t *>(a->structureInput);
    uint8_t *snapshot = nullptr;
    if (input) {
        snapshot = static_cast<uint8_t *>(IOMalloc(size));
        if (!snapshot) return kIOReturnNoMemory;
        status = input->prepare();
        if (status == kIOReturnSuccess) {
            const bool copied = input->readBytes(0, snapshot, size) == size;
            status = input->complete();
            if (!copied) status = kIOReturnVMError;
        }
        au = snapshot;
    }
    if (status == kIOReturnSuccess)
        status = owner->decodeAvcStream(getRegistryEntryID(), au, size, output);
    if (snapshot) IOFree(snapshot, size);
    if (status == kIOReturnSuccess) {
        if (!avcProvider) {
            owner->retain();
            avcProvider = owner;
        }
        a->structureOutputDescriptorSize = capacity;
    }
    return status;
}

IOReturn IOAccelPhxNativeClient::doEncodeH264(IOExternalMethodArguments *a, bool sized) {
    IOMemoryDescriptor *input = a->structureInputDescriptor;
    IOMemoryDescriptor *output = a->structureOutputDescriptor;
    // Supported buffers are larger than IOKit's in-band limit. Only the published
    // out-of-line length is copied back by IOConnectCallStructMethod.
    const uint32_t capacity = a->structureOutputDescriptorSize;
    const uint32_t inlineCapacity = a->structureOutputSize;
    a->structureOutputDescriptorSize = 0;
    a->structureOutputSize = 0;
    EncodeGeometry g;
    if (a->scalarInputCount != (sized ? 2u : 0u)) return kIOReturnBadArgument;
    if (sized && (!a->scalarInput || a->scalarInput[0] > UINT32_MAX || a->scalarInput[1] > UINT32_MAX ||
                  !sizedEncodeGeometry(static_cast<uint32_t>(a->scalarInput[0]),
                                       static_cast<uint32_t>(a->scalarInput[1]), g)))
        return kIOReturnBadArgument;
    if (a->asyncWakePort || a->asyncReferenceCount || a->structureVariableOutputData ||
        a->scalarOutputCount || a->structureInputSize || inlineCapacity ||
        capacity != g.bitstreamBytes)
        return kIOReturnBadArgument;
    return owner->encodeH264Descriptors(input, output, g, &a->structureOutputDescriptorSize);
}

IOReturn PhxBringup::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2,
                                          void *p3, void *p4) {
    const bool closeStream = name && name->isEqualTo(PHX_VIDEO_DECODE_STREAM_CLOSE);
    if (closeStream || (name && name->isEqualTo(PHX_VIDEO_DECODE_STREAM_FUNCTION))) {
        if (!avcMediaEnabled) return kIOReturnUnsupported;
        auto *context = OSDynamicCast(IOUserClient, static_cast<OSObject *>(p1));
        if (!context) return kIOReturnBadArgument;
        const uint64_t ownerID = context->getRegistryEntryID();
        if (!ownerID) return kIOReturnBadArgument;
        if (closeStream) {
            if (p2 || p3 || p4) return kIOReturnBadArgument;
            releaseAvcStream(ownerID);
            return kIOReturnSuccess;
        }
        if (!p2 || !p3 || !p4) return kIOReturnBadArgument;
        auto *written = static_cast<uint32_t *>(p4);
        *written = 0;
        const auto *au = static_cast<const PhxVideoDecodeStreamInput *>(p2);
        auto *output = OSDynamicCast(IOMemoryDescriptor, static_cast<OSObject *>(p3));
        if (!output || output->getLength() > PHX_VIDEO_DECODE_STREAM_MAX_OUTPUT) return kIOReturnBadArgument;
        const IOReturn status = decodeAvcStream(ownerID, static_cast<const uint8_t *>(au->bytes), au->length, output,
                                                au->pitch, au->chromaOffset);
        if (status == kIOReturnSuccess) *written = static_cast<uint32_t>(output->getLength());
        return status;
    }
    if (name && name->isEqualTo(PHX_VIDEO_DECODE_FUNCTION)) {
        if (!p1 || !p2 || !p3 || p4) return kIOReturnBadArgument;
        return decodeReferenceIDR(static_cast<const uint8_t *>(p1), static_cast<IOMemoryDescriptor *>(p2),
                                  static_cast<uint32_t *>(p3));
    }
    if (!name || !name->isEqualTo(PHX_VIDEO_AVC_FUNCTION))
        return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
    if (!p1 || !p2 || !p3 || p4) return kIOReturnBadArgument;
    return encodeH264Descriptors(static_cast<IOMemoryDescriptor *>(p1), static_cast<IOMemoryDescriptor *>(p2),
                                 EncodeGeometry{}, static_cast<uint32_t *>(p3));
}

IOReturn PhxBringup::decodeReferenceIDR(const uint8_t *input, IOMemoryDescriptor *output, uint32_t *bytesWritten) {
    static_assert(PHX_VIDEO_DECODE_WIDTH == kDecodeFrameWidth && PHX_VIDEO_DECODE_HEIGHT == kDecodeFrameHeight &&
                  PHX_VIDEO_DECODE_OUTPUT_BYTES == kDecodeTargetBytes, "video context reference IDR layout");
    *bytesWritten = 0;
    if (!output || output->getLength() != kDecodeTargetBytes) return kIOReturnBadArgument;
    // IOUserClient owns the fixed-size, in-line message until this synchronous call returns.
    if (!isDecodeReferenceIDR(input, PHX_VIDEO_DECODE_INPUT_BYTES)) return kIOReturnUnsupported;
    uint8_t *pixels = static_cast<uint8_t *>(IOMalloc(kDecodeTargetBytes));
    if (!pixels) return kIOReturnNoMemory;
    uint64_t frame[8];
    IOReturn status = vcnCodecFrame(false, pixels, kDecodeTargetBytes, frame, nullptr, {}, input);
    if (status == kIOReturnSuccess && frame[0] != PHX_VCN_OK)
        status = (frame[0] == PHX_VCN_NOT_READY || frame[0] == PHX_VCN_FRAME_REGION) ? kIOReturnNotReady : kIOReturnIOError;
    if (status == kIOReturnSuccess && frame[6] != kDecodeTargetBytes) status = kIOReturnIOError;
    if (status == kIOReturnSuccess) status = output->prepare();
    if (status == kIOReturnSuccess) {
        const IOByteCount copied = output->writeBytes(0, pixels, kDecodeTargetBytes);
        status = output->complete();
        if (copied != kDecodeTargetBytes) status = kIOReturnVMError;
        if (status == kIOReturnSuccess) *bytesWritten = kDecodeTargetBytes;
    }
    IOFree(pixels, kDecodeTargetBytes);
    return status;
}

IOReturn PhxBringup::encodeH264Descriptors(IOMemoryDescriptor *input, IOMemoryDescriptor *output,
                                           const EncodeGeometry &g, uint32_t *bytesWritten) {
    *bytesWritten = 0;
    const uint32_t inputBytes = g.width * g.height * 3u / 2u;
    const uint32_t capacity = g.bitstreamBytes;
    if (!input || input->getLength() != inputBytes || !output || output->getLength() != capacity)
        return kIOReturnBadArgument;

    const uint32_t stagingBytes = inputBytes > capacity ? inputBytes : capacity;
    uint8_t *staging = static_cast<uint8_t *>(IOMalloc(stagingBytes));
    if (!staging)
        return kIOReturnNoMemory;
    IOReturn r = input->prepare();
    if (r == kIOReturnSuccess) {
        IOByteCount copied = input->readBytes(0, staging, inputBytes);
        r = input->complete();
        if (copied != inputBytes)
            r = kIOReturnVMError;
    }
    uint64_t result[8];
    if (r == kIOReturnSuccess) {
        // Once the packed pixels have been copied to the device the same private
        // buffer can receive the stream. Never expose it until close also succeeds.
        r = vcnCodecFrame(true, staging, capacity, result, staging, g);
        if (r == kIOReturnSuccess && result[0] != PHX_VCN_OK)
            r = (result[0] == PHX_VCN_NOT_READY || result[0] == PHX_VCN_FRAME_REGION)
                ? kIOReturnNotReady : kIOReturnIOError;
        if (r == kIOReturnSuccess && (!result[6] || result[6] > capacity))
            r = kIOReturnIOError;
    }
    if (r == kIOReturnSuccess)
        r = output->prepare();
    if (r == kIOReturnSuccess) {
        const uint32_t size = static_cast<uint32_t>(result[6]);
        IOByteCount copied = output->writeBytes(0, staging, size);
        r = output->complete();
        if (copied != size)
            r = kIOReturnVMError;
        if (r == kIOReturnSuccess)
            *bytesWritten = size;
    }
    IOFree(staging, stagingBytes);
    return r;
}

// Selector 120: VCN 4.0.2 bring-up (root + phxdebug only). op 1 status, 2 init, 3 ring test, 4 power down; see
// PHXN_SEL_DBG_VCN in PhxNativeABI.h for the eight output words.
IOReturn IOAccelPhxNativeClient::doDbgVcn(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 8, 0, 0) && !shape(a, 5, 8, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    IOReturn r;
    // op 2 carries the debug stop stage in bits 15:8 (0 = full init)
    // op 2: bits 15:8 debug stop stage (0 = full init), bits 17:16 breadcrumb mode (0 read-back, 1 fast, 2 off)
    // op 2 also carries bits 21 (--sram-full), 23:22 reserved and 31:24 (--fw-type) folded into the stage word.
    const uint32_t op = static_cast<uint32_t>(a->scalarInput[0] & 0xFFu);
    if (op == 2u) {
        if (((a->scalarInput[0] >> 8) & 0xFFu) > 8u || ((a->scalarInput[0] >> 16) & 3u) == 3u)
            return kIOReturnBadArgument;
    } else if (a->scalarInput[0] != op) {
        return kIOReturnBadArgument;
    }
    switch (op) {
    case 1:
        r = owner->vcnStatus(out);
        break;
    case 2: {
        if (a->scalarInput[1] > 0xFFFFFFFFull || a->scalarInput[2] > 0xFFFFFFFFull ||
            a->scalarInput[3] > 0xFFFFFFFFull || a->scalarInput[4] > 0xFFFFFFFFull)
            return kIOReturnBadArgument;
        const uint64_t tmr = (a->scalarInput[4] << 32) | a->scalarInput[3];
        r = owner->vcnInit(static_cast<uint32_t>(a->scalarInput[1]), static_cast<uint32_t>(a->scalarInput[2]), tmr,
                           static_cast<uint32_t>((a->scalarInput[0] >> 8) & 0x1FFFFFF),
                           a->scalarInputCount == 6 ? a->scalarInput[5] : 0, out);
        break;
    }
    case 3:
        r = owner->vcnRingTest(out);
        break;
    case 4:
        r = owner->vcnShutdown(out);
        break;
    case 5:
        r = owner->vcnFenceTest(out);
        break;
    case 6:
        if (a->scalarInput[1] > 0xFFFu)
            return kIOReturnBadArgument;
        r = owner->vcnSetDoorbell(static_cast<uint32_t>(a->scalarInput[1]));
        break;
    case 7: {   // LOAD_TOC with the TOC already staged in fw_buf
        if (a->scalarInput[1] > 0xFFFFFFFFull || a->scalarInput[2] > 0xFFFFFFFFull)
            return kIOReturnBadArgument;
        r = owner->vcnLoadToc(static_cast<uint32_t>(a->scalarInput[1]),
                              static_cast<uint32_t>(a->scalarInput[2]), out);
        break;
    }
    case 8: case 9:
        r = owner->vcnCodecSessionTest(a->scalarInput[0] == 9, out);
        break;
    case 10: case 11: {
        if (a->scalarInputCount != 6 || a->scalarInput[1] > UINT32_MAX || a->scalarInput[2] ||
            a->scalarInput[3] || a->scalarInput[4] || a->scalarInput[5])
            return kIOReturnBadArgument;
        Bo *bo = findBo(static_cast<uint32_t>(a->scalarInput[1]));
        const uint32_t required = op == 11 ? kEncodeFrameBitstreamBytes : kDecodeTargetBytes;
        if (!bo || bo->host || !bo->kva || bo->size < required)
            return kIOReturnBadArgument;
        r = owner->vcnCodecFrame(op == 11, bo->kva, required, out);
        break;
    }
    default:
        return kIOReturnBadArgument;
    }
    for (uint32_t i = 0; i < 8u; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

// Write back and invalidate the CPU caches over every BO: CPU-written data must be in memory before the GPU reads
// it, and the CPU must not read stale lines after the GPU wrote. Coarse: WindowServer's BOs (full-screen surfaces
// among them) made this its hottest loop, hence debug.phxbos_flush.
void IOAccelPhxNativeClient::flushBos() {
    const int mode = gBosFlush;
    if (!mode)
        return;
    uint64_t bytes = 0;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++) {
        if (!bos[h].kva)   // host imports: the CPU's own pages, which the GPU reads and writes snooped
            continue;
        const uint8_t *p = bos[h].kva;
        if (mode == 1) {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
        } else {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflush (%0)" ::"r"(p + o) : "memory");
        }
        bytes += bos[h].size;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    OSAddAtomic64((SInt64)bytes, &gBosFlushBytes);
}

// WindowServer's composite marks for PhxAccel: the swap of a surface marked pending waits in NMXAccel until it is marked
// ready (nvSurfWaitReady), so WindowServer need not wait for each composite itself (commitAndWaitUntilSubmitted held
// its main thread ~1.7 ms a frame, 2026-10-26). They reached NMXAccel through NVIDIA's RM escape before; here the
// accelerator's own entry (callPlatformFunction "nvSurfVram", NVRM_ESC_SURF_DIRTY with the GPU-mark plane bit) takes
// them. The accelerator checks the caller is WindowServer.
struct PhxSurfShareEsc {   // NVRMSurfShareEsc, src/phxaccel/nvrm_surfshare_abi.h (version 2)
    uint32_t version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad;
    uint64_t size;
};
static_assert(sizeof(PhxSurfShareEsc) == 40, "NVRMSurfShareEsc layout");
IOReturn IOAccelPhxNativeClient::doSurfGpuMark(IOExternalMethodArguments *a) {
    // Third scalar is optional on old plugins: 0 = legacy one-bit mark, the accelerator assigns and returns a number.
    if (!shape(a, 2, 1, 0, 0) && !shape(a, 3, 1, 0, 0))
        return kIOReturnBadArgument;
    static IOService *accel;
    static const OSSymbol *fn;
    if (!accel) {
        IOService *s = IOService::copyMatchingService(IOService::serviceMatching("NMXAccel"));
        if (s && !OSCompareAndSwapPtr(nullptr, s, (void *volatile *)&accel))
            s->release();
        if (!accel)
            return kIOReturnNotFound;
    }
    if (!fn) {
        const OSSymbol *f = OSSymbol::withCStringNoCopy("nvSurfVram");
        if (f && !OSCompareAndSwapPtr(nullptr, (void *)f, (void *volatile *)&fn))
            f->release();
        if (!fn)
            return kIOReturnNoMemory;
    }
    PhxSurfShareEsc e{};
    e.version = 2;
    e.surfaceID = static_cast<uint32_t>(a->scalarInput[0]);
    const uint32_t seq = a->scalarInputCount >= 3 ? static_cast<uint32_t>(a->scalarInput[2]) : 0u;
    e.plane = 0x80000000u | (a->scalarInput[1] ? 1u : 0u) | ((seq << 2) & 0x3ffffffcu);
    // NVRM_SS_PLANE_GPU_MARK | pending | numbered mark (bits 2..29)
    e.status = 0x80000002u;                                    // NVRM_SS_OFF unless the accelerator answers
    const IOReturn r = accel->callPlatformFunction(fn, false, &e, task, reinterpret_cast<void *>(0xE3), nullptr);   // NVRM_ESC_SURF_DIRTY
    a->scalarOutput[0] = r == kIOReturnSuccess ? e.status : 0x80000002u;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doWaitSeq(IOExternalMethodArguments *a) {
    if (!hello)
        return kIOReturnNotReady;
    if (!shape(a, 3, 3, 0, 0))
        return kIOReturnBadArgument;
    IOLockLock(lock);
    uint64_t target = a->scalarInput[0];
    const uint64_t em = emitted;
    IOLockUnlock(lock);
    if (target == PHXN_SEQ_LAST)
        target = em;
    if (target > em)
        return kIOReturnBadArgument;
    if (owner->gpuReady()) {
        // E8: poll the RELEASE_MEM fence the adopted queue writes after each submission
        uint64_t timeout = a->scalarInput[1];
        if (timeout > PHXN_WAIT_CAP_NS)
            timeout = PHXN_WAIT_CAP_NS;
        uint64_t retired = owner->gpuRetired();
        for (uint64_t waited = 0; retired < target && waited < timeout; ) {
            if (waited < 1000000ull) {
                // Short wait (< 1 ms): fast poll with 50 us delay for low latency
                IODelay(50);
                waited += 50000ull;
            } else {
                // Extended wait (>= 1 ms): yield CPU to scheduler to avoid starving WindowServer & system threads
                IOSleep(1);
                waited += 1000000ull;
            }
            retired = owner->gpuRetired();
        }
        if (retired >= target) {
            // drop CPU cache lines so the CPU sees what the GPU wrote; under the lock, as another thread of the same
            // client (WindowServer) freed a BO mid-flush and the walk dereferenced its NULL descriptor (panic 2026-10-20)
            IOLockLock(lock);
            flushBos();
            if (pendFenceSeq && pendFenceSeq <= retired) {
                Bo *fb = findBo(pendFenceHandle);
                if (fb && fb->kva && pendFenceOffset + 8 <= fb->size)
                    bcopy(&pendFenceSeq, fb->kva + pendFenceOffset, 8);
                pendFenceSeq = 0;
            }
            IOLockUnlock(lock);
        }
        a->scalarOutput[0] = retired < target;
        a->scalarOutput[1] = retired;
        a->scalarOutput[2] = em;
        return kIOReturnSuccess;
    }
    a->scalarOutput[0] = 0;   // not busy: E1 retires every submission at once
    a->scalarOutput[1] = em;
    a->scalarOutput[2] = em;
    return kIOReturnSuccess;
}
