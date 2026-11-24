/*
 * PhxVcn.h — hard-coded VCN 4.0.2 (Phoenix / Radeon 780M) bring-up constants.
 *
 * Every number here is taken from the VCN instance 0 discovered on this machine (IP discovery:
 * VCN v4.0.2, bases 0x7800 / 0x7e00 dwords) and from the upstream Linux sources that drive
 * exactly this block: drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c, amdgpu_vcn.{c,h},
 * include/asic_reg/vcn/vcn_4_0_0_{offset,sh_mask}.h (all MIT).
 *
 * Nothing here is discovered at runtime: Phoenix has one VCN instance at a fixed address, and the
 * register set is fixed. tests/vcn_regs_check.py re-parses the upstream register headers and
 * compares them with the values below, so a typo cannot survive the test.
 *
 * This header is self-contained (stdint + macros only) so both the kext and host tests can use it.
 */
#ifndef PHX_VCN_H
#define PHX_VCN_H

#include <stdint.h>

namespace phx_vcn {

// ---------------------------------------------------------------------------------------------------------------
// Register block. SOC15 maps VCN instance 0 to dword offsets 0x7800 (segment 0) and 0x7e00 (segment 1); the
// per-register offsets below already include their segment base (that is what regUVD_*_BASE_IDX selects).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kBase0 = 0x7800u;   // VCN segment 0
constexpr uint32_t kBase1 = 0x7e00u;   // VCN segment 1

constexpr uint32_t kRegPgfsmConfig     = kBase1 + 0x0000u;   // 0x7e00
constexpr uint32_t kRegPgfsmStatus     = kBase1 + 0x0001u;   // 0x7e01
constexpr uint32_t kRegPowerStatus     = kBase1 + 0x0002u;   // 0x7e02
constexpr uint32_t kRegUvdStatus       = kBase0 + 0x0743u;   // 0x7f43
constexpr uint32_t kRegVcpuCntl        = kBase0 + 0x0796u;   // 0x7f96
constexpr uint32_t kRegMastintEn       = kBase1 + 0x00e1u;   // 0x7ee1
constexpr uint32_t kRegLmiCtrl2        = kBase0 + 0x08aeu;   // 0x80ae
constexpr uint32_t kRegSoftReset       = kBase0 + 0x0747u;   // 0x7f47
constexpr uint32_t kRegLmiCtrl         = kBase0 + 0x08b0u;   // 0x80b0
constexpr uint32_t kRegMpcCntl         = kBase0 + 0x07d4u;   // 0x7fd4
constexpr uint32_t kRegMpcSetMuxA0     = kBase0 + 0x07d6u;   // 0x7fd6
constexpr uint32_t kRegMpcSetMuxB0     = kBase0 + 0x07d8u;   // 0x7fd8
constexpr uint32_t kRegMpcSetMux       = kBase0 + 0x07dau;   // 0x7fda
constexpr uint32_t kRegLmiVcpuCacheBarLo   = kBase0 + 0x0844u;   // 0x8044
constexpr uint32_t kRegLmiVcpuCacheBarHi   = kBase0 + 0x0845u;   // 0x8045
constexpr uint32_t kRegVcpuCacheOffset0    = kBase0 + 0x0780u;   // 0x7f80
constexpr uint32_t kRegVcpuCacheSize0      = kBase0 + 0x0781u;   // 0x7f81
constexpr uint32_t kRegLmiVcpuCache1BarLo  = kBase0 + 0x0870u;   // 0x8070
constexpr uint32_t kRegLmiVcpuCache1BarHi  = kBase0 + 0x0871u;   // 0x8071
constexpr uint32_t kRegVcpuCacheOffset1    = kBase0 + 0x0782u;   // 0x7f82
constexpr uint32_t kRegVcpuCacheSize1      = kBase0 + 0x0783u;   // 0x7f83
constexpr uint32_t kRegLmiVcpuCache2BarLo  = kBase0 + 0x0874u;   // 0x8074
constexpr uint32_t kRegLmiVcpuCache2BarHi  = kBase0 + 0x0875u;   // 0x8075
constexpr uint32_t kRegVcpuCacheOffset2    = kBase0 + 0x0784u;   // 0x7f84
constexpr uint32_t kRegVcpuCacheSize2      = kBase0 + 0x0785u;   // 0x7f85
constexpr uint32_t kRegLmiVcpuNc0BarLo     = kBase0 + 0x0840u;   // 0x8040
constexpr uint32_t kRegLmiVcpuNc0BarHi     = kBase0 + 0x0841u;   // 0x8041
constexpr uint32_t kRegVcpuNoncacheOffset0 = kBase0 + 0x0792u;   // 0x7f92
constexpr uint32_t kRegVcpuNoncacheSize0   = kBase0 + 0x0793u;   // 0x7f93
constexpr uint32_t kRegCgcGate          = kBase1 + 0x00c1u;   // 0x7ec1
constexpr uint32_t kRegCgcCtrl          = kBase1 + 0x00c2u;   // 0x7ec2
constexpr uint32_t kRegSuvdCgcGate      = kBase1 + 0x00c4u;   // 0x7ec4
constexpr uint32_t kRegSuvdCgcCtrl      = kBase1 + 0x00c6u;   // 0x7ec6
constexpr uint32_t kRegScratch1            = kBase1 + 0x0015u;   // 0x7e15: the firmware writes its version here
constexpr uint32_t kRegDpgLmaCtl          = kBase1 + 0x0011u;   // 0x7e11
constexpr uint32_t kRegDpgLmaData         = kBase1 + 0x0012u;   // 0x7e12
constexpr uint32_t kRegDpgLmaMask         = kBase1 + 0x0013u;   // 0x7e13
constexpr uint32_t kRegGfx10AddrConfig     = kBase1 + 0x0042u;   // 0x7e42
constexpr uint32_t kRegRbArbCtrl           = kBase0 + 0x0706u;   // 0x7f06
constexpr uint32_t kRegRb1DbCtrl           = kBase1 + 0x0072u;   // 0x7e72
constexpr uint32_t kRegRbBaseLo            = kBase1 + 0x00eau;   // 0x7eea
constexpr uint32_t kRegRbBaseHi            = kBase1 + 0x00ebu;   // 0x7eeb
constexpr uint32_t kRegRbSize              = kBase1 + 0x00ecu;   // 0x7eec
constexpr uint32_t kRegRbEnable            = kBase1 + 0x0085u;   // 0x7e85
constexpr uint32_t kRegRbRptr              = kBase1 + 0x00acu;   // 0x7eac
constexpr uint32_t kRegRbWptr              = kBase1 + 0x00adu;   // 0x7ead

// ---------------------------------------------------------------------------------------------------------------
// Field values (vcn_4_0_0_sh_mask.h and the engine_status_constants enum in amdgpu_vcn.h).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kPgfsmStatusMask   = 0x3F3FFFFFu;
// PGFSM plane encoding (vcn_v4_0.c): status 0 = plane ON, 2 = plane OFF. The SMU's PowerUpVcn leaves every plane
// off (0x2AAAAAAA); disable_static_power_gating turns M/S/LMI on and must see 0x2A2A8AA0 before any other VCN
// register is touched -- reading UVD_STATUS with the planes off hard-resets the machine (seen 2026-11-21).
constexpr uint32_t kPgfsmPoweredNoPg  = 0x00000000u;   // non-PG path, instance 0: every plane on
constexpr uint32_t kPgfsmPoweredPg    = 0x2A2A8AA0u;   // UVD_PGFSM_STATUS__UVDM_UVDU_UVDLM_PWR_ON_3_0
constexpr uint32_t kPgfsmGated        = 0x2A2AAAAAu;   // every plane off (enable_static_power_gating wait)
constexpr uint32_t kPgfsmConfigNoPg   = 0x15155555u;   // non-PG path: every plane = 1
constexpr uint32_t kPgfsmConfigPg     = 0x2A2A9AA5u;   // PG path (Phoenix has AMD_PG_SUPPORT_VCN): M/S/LMI = 1, rest = 2
constexpr uint32_t kPgfsmConfigGate   = 0x2A2AAAAAu;   // enable_static_power_gating: every plane = 2
constexpr uint32_t kPowerStatusKeep   = ~0x103u;       // Linux: data &= ~0x103
constexpr uint32_t kPowerStatusPgOn   = 0x2u | 0x100u; // UVDM_UVDU_PWR_ON | UVD_PG_EN
constexpr uint32_t kPowerStatusMask   = 0x3u;          // UVD_POWER_STATUS__UVD_POWER_STATUS_MASK
constexpr uint32_t kPowerStatusTilesOff = 0x1u;        // UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF
// Read back after a VCPU boot timeout (vcn_4_0_0_offset.h, BASE_IDX 1 = 0x7e00 + offset).
constexpr uint32_t kVcnDiagRegs[] = {
    0x7f43u /* UVD_STATUS */, 0x7f96u /* VCPU_CNTL */, 0x7f47u /* SOFT_RESET */, 0x7f06u /* RB_ARB_CTRL */,
    0x7ee1u /* MASTINT_EN */, 0x80aeu /* LMI_CTRL2 */, 0x80b0u /* LMI_CTRL */, 0x80b1u /* LMI_STATUS */,
    0x7ec1u /* CGC_GATE */, 0x7ec2u /* CGC_CTRL */, 0x7ec4u /* SUVD_CGC_GATE */, 0x7e02u /* POWER_STATUS */,
    0x7f81u /* VCPU_CACHE_SIZE0 */, 0x8044u /* CACHE_BAR_LO */, 0x8045u /* CACHE_BAR_HI */,
    0x7ed0u /* GPCOM_VCPU_DATA0 */, 0x7e15u /* SCRATCH1 */, 0x7e01u /* PGFSM_STATUS */,
};
// Register sweep of VCN's two segments (0x7e00..0x80ff), written to VRAM after a VCPU boot timeout: addresses that
// read all-ones are not backed by hardware (mis-mapped or in an unpowered sub-block). Read-only.
constexpr uint32_t kSweepOff   = 0xC940000u;
constexpr uint32_t kSweepFirst = 0x7e00u;
constexpr uint32_t kSweepCount = 0x0300u;
constexpr uint32_t kSweepMagic = 0x53574356u;   // "CVWS"
// DPG-mode constants (vcn_v4_0_start_dpg_mode). The VCPU-side registers are written through UVD_DPG_LMA_* rather
// than directly: direct writes to them do not take on this chip (they read back all-ones), the port does not.
constexpr uint32_t kDpgVidSocBase = 0x1FB00u;   // VCN_VID_SOC_ADDRESS_2_0 (bytes, 20-bit window)
constexpr uint32_t kDpgAonSocBase = 0x1F800u;   // VCN_AON_SOC_ADDRESS_2_0
constexpr uint32_t kDpgAonIpBase  = 0x30000u;   // VCN_AON_IP_ADDRESS_2_0
constexpr uint32_t kPowerStatusPgEn = 0x00000100u;   // UVD_POWER_STATUS__UVD_PG_EN_MASK
// Phoenix NBIF 7.7.0, from IP discovery and the Linux VCN0 range programming.
constexpr uint32_t kRsmuIndex = 0x14u;
constexpr uint32_t kRsmuData = 0x15u;
constexpr uint32_t kDoorbellApertureReg = 0xDE0u; // NBIO 7.7.0 base[2] + RCC aperture enable
constexpr uint32_t kVcnDoorbellRangeAddr = (0x10400u + 0x4f0af3u) * 4u;
constexpr uint32_t kDbRangeStart = 0x310u;
constexpr uint32_t kVcnDoorbellFields = 0x001F0FFCu; // NBIO 7.7.0 SIZE_MASK | OFFSET_MASK
constexpr uint32_t kDbRangeCount = 8u;
constexpr uint32_t kVcnDoorbellRange = (kDbRangeCount << 16) | (kDbRangeStart << 2);
constexpr uint32_t kDbIndexLinux = 0x312u;
constexpr uint32_t kPowerStatusPgMode = 0x00000004u;   // UVD_POWER_STATUS__UVD_PG_MODE_MASK
constexpr uint32_t kDpgLmiCtrl    = 0x00307308u;       // WRITE_CLEAN_TIMER_EN|REQ_MODE|CRC_RESET|MASK_MC_URGENT|
                                                       // DATA_COHERENCY_EN|VCPU_DATA_COHERENCY_EN|8|0x100000
constexpr uint32_t kDpgLmiCtrl2   = 0x003E0000u;       // 0x1f << RE_OFLD_MIF_WR_REQ_NUM_SHIFT
constexpr uint32_t kDpgMpcCntl    = 0x00000010u;       // 0x2 << REPLACEMENT_MODE_SHIFT
// vcn_v4_0_disable_clock_gating. The masks are the OR of the fields Linux clears or sets, taken from
// vcn_4_0_0_sh_mask.h; kCgcCtrlTiming is 1 << CLK_GATE_DLY_TIMER | 4 << CLK_OFF_DELAY.
constexpr uint32_t kCgcCtrlDynMode     = 0x00000001u;   // UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK
constexpr uint32_t kCgcCtrlTiming      = 0x00000104u;
constexpr uint32_t kCgcGateClear       = 0x0017ffffu;   // SYS..MMSCH (20 gate fields)
constexpr uint32_t kCgcCtrlClear       = 0xbffff800u;   // the same 20 fields' *_MODE bits (DYN_CLOCK_MODE kept out)
constexpr uint32_t kSuvdCgcGateSet     = 0x01feffffu;   // 24 SUVD gate fields
constexpr uint32_t kSuvdCgcCtrlClear   = 0x000003ffu;   // 10 SUVD *_MODE fields
constexpr uint32_t kUvdStatusBusy     = 0x00000004u;   // UVD_STATUS__UVD_BUSY
constexpr uint32_t kUvdStatusVcpuRdy  = 0x00000002u;   // bit 1: VCPU reported firmware ready
constexpr uint32_t kVcpuCntlClkEn     = 0x00000200u;
constexpr uint32_t kVcpuCntlBlkRst    = 0x10000000u;
// UVD_VCPU_CNTL reads all-ones for every reserved bit, so it must be written outright (never read-modify-write).
// vcn_v4_0_start_dpg_mode: (0xFF << PRB_TIMEOUT_VAL) | CLK_EN | BLK_RST, then the same value without BLK_RST.
constexpr uint32_t kVcpuCntlHold      = 0x1FF00200u;   // (0xFF << 20) | CLK_EN | BLK_RST: VCPU held in reset
constexpr uint32_t kVcpuCntlRun       = 0x0FF00200u;   // (0xFF << 20) | CLK_EN: BLK_RST released
constexpr uint32_t kMastintEnVcpuEn   = 0x00000002u;
constexpr uint32_t kLmiCtrl2StallUmc  = 0x00000100u;
constexpr uint32_t kSoftResetLmi      = 0x00000004u;
constexpr uint32_t kSoftResetLmiUmc   = 0x00002000u;
constexpr uint32_t kLmiCtrlFlags      = 0x00000100u | 0x00001000u | 0x00002000u | 0x00200000u;
constexpr uint32_t kMpcCntlReplMask   = 0x00000038u;
constexpr uint32_t kMpcCntlReplShift  = 3u;
constexpr uint32_t kMpcCntlReplMode   = 2u;
constexpr uint32_t kRbArbCtrlVcpuDis  = 0x00000008u;
constexpr uint32_t kRb1DbCtrlOffsetShift = 2u;
constexpr uint32_t kRb1DbCtrlEn       = 0x40000000u;
constexpr uint32_t kRbEnableRb1       = 0x00000004u;

// UVD_MPC_SET_MUX{A,B}0 / SET_MUX constant data (vcn_v4_0_start): 1,2,3,4 in the four VAR fields.
constexpr uint32_t kMpcSetMuxA =
    (1u << 6) | (2u << 12) | (3u << 18) | (4u << 24);
constexpr uint32_t kMpcSetMuxB =
    (1u << 6) | (2u << 12) | (3u << 18) | (4u << 24);
constexpr uint32_t kMpcSetMux = (0u << 0) | (1u << 3) | (2u << 6);

// fw_shared (struct amdgpu_vcn4_fw_shared, sizeof 148, page-aligned by Linux). Offsets are byte offsets.
constexpr uint32_t kSharedPresentFlags = 0u;     // present_flag_0 precedes 12 reserved bytes
constexpr uint32_t kSharedSq           = 16u;   // is_enabled (+0), queue_mode (+1), queue_status (+2)
constexpr uint32_t kSharedSqEnabled    = 1u;
constexpr uint32_t kSharedSmuInterface = 120u;  // smu_interface_type = 1 (APU)
constexpr uint32_t kSharedDrmKey       = 124u;  // method = 2 (VCNFW/ASD handshaking workaround)
constexpr uint32_t kSharedSize         = 4096u; // AMDGPU_GPU_PAGE_ALIGN(sizeof(...))

constexpr uint32_t kFwFlagUnifiedQueue = 1u << 2;
constexpr uint32_t kFwFlagDrmKeyInject = 1u << 4;
constexpr uint32_t kFwFlagSmuDpm       = 1u << 11;
constexpr uint32_t kFwDrmKeyMethod     = 2u;
constexpr uint32_t kFwSmuInterfaceApu  = 1u;
constexpr uint32_t kFwQueueRingReset   = 1u;
constexpr uint32_t kFwQueueDpgHoldOff  = 2u;

// Unified ring (amdgpu_ring_init(..., 512 dw)): 512 dwords = 2048 B, 64-byte aligned, metadata 36 B after it.
constexpr uint32_t kRingDw     = 512u;
constexpr uint32_t kRingBytes  = kRingDw * 4u;
constexpr uint32_t kRingAlign  = 64u;
constexpr uint32_t kRbMetadataBytes = 36u;
constexpr uint32_t kRingCommitAlignDw = 64u;     // vcn_v4_0_unified_ring_vm_funcs.align_mask + 1
constexpr uint32_t kCmdNop     = 0x00000000u;   // VCN_ENC_CMD_NO_OP
constexpr uint32_t kCmdEnd     = 0x00000001u;   // VCN_ENC_CMD_END
constexpr uint32_t kCmdFence   = 0x00000003u;   // VCN_ENC_CMD_FENCE
constexpr uint32_t kFenceDwords = 5u;           // FENCE, addr lo/hi, seq, END (polled, no interrupt)

// VRAM layout of the per-instance block (byte offsets inside the 256 MiB BAR0 aperture):
// stack 128 KiB, context 512 KiB, fw_shared 4 KiB, ring 2 KiB + metadata. 200 MiB leaves the 64 MiB TMR
// (0x8000000..0xC000000) and everything below it untouched.
constexpr uint32_t kBoOffset  = 0xC800000u;   // 200 MiB
constexpr uint32_t kBoSize    = 0x100000u;    // 1 MiB
constexpr uint32_t kStackOff  = 0x00000u;
constexpr uint32_t kStackSize = 128u * 1024u;
constexpr uint32_t kCtxOff    = kStackOff + kStackSize;   // 0x20000
constexpr uint32_t kCtxSize   = 512u * 1024u;
constexpr uint32_t kSharedOff = kCtxOff + kCtxSize;       // 0xA0000
constexpr uint32_t kRingOff   = kSharedOff + kSharedSize; // 0xA1000
constexpr uint32_t kMetaOff   = kRingOff + kRingBytes;    // 0xA1800
constexpr uint32_t kSramOff   = 0xB1000u;                 // 512 B DPG command list (64 offset/value pairs)
constexpr uint32_t kPspFwTypeVcnRam = 49u;             // GFX_FW_TYPE_VCN0_RAM
constexpr uint32_t kWbOff     = 0xB0000u;                 // 32-bit fence value the firmware writes
constexpr uint32_t kWbSize    = 4u;

// Private, synchronous session diagnostics in unused pages of the existing per-instance BO.
constexpr uint32_t kCodecIbOff = 0xB3000u;
constexpr uint32_t kCodecDataOff = 0xB4000u;
constexpr uint32_t kEncodeContextBytes = 128u * 1024u;
static_assert(kSramOff + 8192u <= kCodecIbOff && kCodecIbOff + 4096u <= kCodecDataOff &&
              kCodecDataOff + kEncodeContextBytes <= kBoSize, "VCN diagnostic buffers overlap");

// PSP LOAD_IP_FW firmware type for VCN (psp_gfx_if.h: GFX_FW_TYPE_VCN = 13).
constexpr uint32_t kPspFwTypeVcn = 13u;
// SMU 13.0.4 messages (smu_v13_0_4_ppsmc.h).
constexpr uint32_t kPpsmcPowerDownVcn = 0x06u;
constexpr uint32_t kPpsmcPowerUpVcn   = 0x07u;
constexpr uint32_t kPpsmcResultOk     = 0x01u;

// common_firmware_header (amdgpu_ucode.h), the 32 bytes at the start of vcn_4_0_0.bin.
struct FwHeader {
    uint32_t sizeBytes;
    uint32_t headerSizeBytes;
    uint16_t headerVersionMajor;
    uint16_t headerVersionMinor;
    uint16_t ipVersionMajor;
    uint16_t ipVersionMinor;
    uint32_t ucodeVersion;
    uint32_t ucodeSizeBytes;
    uint32_t ucodeArrayOffsetBytes;
    uint32_t crc32;
};
static_assert(sizeof(FwHeader) == 32, "common_firmware_header is 32 bytes");

// Rejects anything that is not the VCN 4.0 image this block runs: header v1.0, IP 4.0, a 4-byte aligned
// ucode array that lies inside the buffer, and a non-empty ucode that ends inside it. Pure function so the
// host test can feed it the real vcn_4_0_0.bin header plus a set of broken ones.
inline bool fwHeaderOk(const FwHeader &h, uint32_t fileLen, uint32_t *ucodeOff, uint32_t *ucodeSize) {
    if (h.headerSizeBytes != 32u || h.headerVersionMajor != 1u || h.headerVersionMinor != 0u)
        return false;
    if (h.ipVersionMajor != 4u || h.ipVersionMinor != 0u)
        return false;
    if (h.ucodeArrayOffsetBytes < h.headerSizeBytes || (h.ucodeArrayOffsetBytes & 3u))
        return false;
    if (h.ucodeSizeBytes == 0u || h.ucodeSizeBytes > (16u << 20))
        return false;
    // 64-bit arithmetic: a corrupt header must not wrap the bounds check.
    const uint64_t end = static_cast<uint64_t>(h.ucodeArrayOffsetBytes) + h.ucodeSizeBytes;
    if (h.sizeBytes != fileLen || end > fileLen)
        return false;
    *ucodeOff = h.ucodeArrayOffsetBytes;
    *ucodeSize = h.ucodeSizeBytes;
    return true;
}

// SOC15_DPG_MODE_OFFSET from amdgpu_vcn.h: the internal (VCPU-side) address of an MMIO register.
inline uint32_t dpgModeOffset(uint32_t reg) {
    const uint32_t low = (reg << 2) & 0xFFFFFu;
    uint32_t internal;
    if (low >= kDpgVidSocBase && low < kDpgVidSocBase + 0x2600u)
        internal = low - kDpgVidSocBase;
    else if (low >= kDpgAonSocBase && low < kDpgAonSocBase + 0x600u)
        internal = low - kDpgAonSocBase + kDpgAonIpBase;
    else
        internal = low;
    return internal >> 2;
}

inline uint32_t firmwareCacheBytes(uint32_t ucodeSize) {
    return (ucodeSize + 8u + 0xFFFu) & ~0xFFFu;
}

// Phoenix uses VCN MGCG and has no VCN RAS writes in the 216-byte command list.
// vcn_v4_0_mc_resume_dpg_mode uses placeholders for the firmware window and stack
// address in a VCN0_RAM list. The direct port needs their actual values.
template<typename Write>
inline void writeDpgSetup(Write write, bool indirect, uint64_t firmware, uint32_t cacheBytes,
                          uint64_t stack, uint64_t context, uint64_t shared, uint32_t addrConfig) {
    write(kRegVcpuCntl, kVcpuCntlHold);
    write(kRegMastintEn, 0);
    write(kRegLmiCtrl, kDpgLmiCtrl);
    write(kRegMpcCntl, kDpgMpcCntl);
    write(kRegMpcSetMuxA0, kMpcSetMuxA);
    write(kRegMpcSetMuxB0, kMpcSetMuxB);
    write(kRegMpcSetMux, kMpcSetMux);
    write(kRegLmiVcpuCacheBarLo, indirect ? 0u : static_cast<uint32_t>(firmware));
    write(kRegLmiVcpuCacheBarHi, indirect ? 0u : static_cast<uint32_t>(firmware >> 32));
    write(kRegVcpuCacheOffset0, 0);
    write(kRegVcpuCacheSize0, indirect ? 0u : cacheBytes);
    write(kRegLmiVcpuCache1BarLo, indirect ? 0u : static_cast<uint32_t>(stack));
    write(kRegLmiVcpuCache1BarHi, indirect ? 0u : static_cast<uint32_t>(stack >> 32));
    write(kRegVcpuCacheOffset1, 0);
    write(kRegVcpuCacheSize1, kStackSize);
    write(kRegLmiVcpuCache2BarLo, static_cast<uint32_t>(context));
    write(kRegLmiVcpuCache2BarHi, static_cast<uint32_t>(context >> 32));
    write(kRegVcpuCacheOffset2, 0);
    write(kRegVcpuCacheSize2, kCtxSize);
    write(kRegLmiVcpuNc0BarLo, static_cast<uint32_t>(shared));
    write(kRegLmiVcpuNc0BarHi, static_cast<uint32_t>(shared >> 32));
    write(kRegVcpuNoncacheOffset0, 0);
    write(kRegVcpuNoncacheSize0, kSharedSize);
    write(kRegGfx10AddrConfig, addrConfig);
}

template<typename Write>
inline void writeDpgRelease(Write write) {
    write(kRegVcpuCntl, kVcpuCntlRun);
    write(kRegLmiCtrl2, kDpgLmiCtrl2);
    write(kRegMastintEn, kMastintEnVcpuEn);
}

inline bool firmwareFitsTmr(uint64_t fwMc, uint32_t cacheBytes, uint64_t tmrMc, uint64_t tmrBytes) {
    return fwMc != 0 && fwMc >= tmrMc && cacheBytes != 0 &&
           cacheBytes <= tmrBytes && fwMc - tmrMc <= tmrBytes - cacheBytes;
}

inline uint32_t ringSlot(uint32_t ptr) { return ptr & (kRingDw - 1u); }

inline uint32_t ringCommitAfter(uint32_t wptr, uint32_t packetDw) {
    return (wptr + packetDw + kRingCommitAlignDw - 1u) & ~(kRingCommitAlignDw - 1u);
}

// The diagnostic ring uses a polled 32-bit fence, so it omits Linux's trailing interrupt TRAP.
inline void buildFencePacket(uint32_t dw[kFenceDwords], uint64_t mc, uint32_t seq) {
    dw[0] = kCmdFence;
    dw[1] = static_cast<uint32_t>(mc);
    dw[2] = static_cast<uint32_t>(mc >> 32);
    dw[3] = seq;
    dw[4] = kCmdEnd;
}

// VCN_ENC_CMD_IB for a driver-owned VRAM allocation, followed by a polled completion fence.
inline void buildIbFencePacket(uint32_t *dw, uint64_t ibMc, uint32_t ibDw, uint64_t fenceMc, uint32_t seq) {
    dw[0] = 2u;
    dw[1] = 0u; // VMID 0
    dw[2] = static_cast<uint32_t>(ibMc);
    dw[3] = static_cast<uint32_t>(ibMc >> 32);
    dw[4] = ibDw;
    buildFencePacket(dw + 5, fenceMc, seq);
}

} // namespace phx_vcn

#endif // PHX_VCN_H
