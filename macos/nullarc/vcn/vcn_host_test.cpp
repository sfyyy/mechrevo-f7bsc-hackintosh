// Host test for the pure parts of the VCN bring-up: the firmware-header check, the VRAM layout arithmetic and
// the constants that are derived rather than copied. Built and run by tests/vcn_selftest.sh; it never touches
// hardware.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../PhxVcn.h"

using namespace phx_vcn;

static int gFailures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); gFailures++; } } while (0)
#define CHECK_EQ(a, b) do { \
    const unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a != _b) { std::printf("FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); gFailures++; } \
} while (0)

static std::vector<unsigned char> readFile(const char *path, bool &ok) {
    ok = false;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> b;
    if (n > 0) {
        b.resize(static_cast<size_t>(n));
        ok = std::fread(b.data(), 1, b.size(), f) == b.size();
    }
    std::fclose(f);
    return b;
}

static FwHeader loadHeader(const std::vector<unsigned char> &b) {
    FwHeader h;
    std::memcpy(&h, b.data(), sizeof(h));
    return h;
}

static void testLayout() {
    // The four windows of the per-instance block must sit inside kBoSize and never overlap.
    CHECK_EQ(kStackOff, 0u);
    CHECK_EQ(kStackOff + kStackSize, kCtxOff);
    CHECK_EQ(kCtxOff + kCtxSize, kSharedOff);
    CHECK_EQ(kSharedOff + kSharedSize, kRingOff);
    CHECK_EQ(kRingOff % kRingAlign, 0u);
    CHECK(kMetaOff + kRbMetadataBytes <= kBoSize);
    CHECK_EQ(kRingDw * 4u, kRingBytes);
    CHECK_EQ(kSharedOff % 4096u, 0u);          // page-aligned, as Linux allocates fw_shared
    CHECK_EQ(kBoOffset % 4096u, 0u);
    // 200 MiB .. 201 MiB: above the 64 MiB TMR (0x8000000..0xC000000), below the 256 MiB BAR0 aperture.
    CHECK_EQ(kBoOffset, 0xC800000u);
    CHECK(kBoOffset >= 0xC000000u);
    CHECK(kBoOffset + kBoSize <= 0x10000000u);

    // fw_shared field offsets from struct amdgpu_vcn4_fw_shared (see the LAYOUT lines in vcn_linux_regs.txt).
    CHECK_EQ(kSharedPresentFlags, 0u);
    CHECK_EQ(kSharedSq, 16u);
    CHECK(kSharedPresentFlags + sizeof(uint32_t) <= kSharedSq);
    CHECK_EQ(kSharedSmuInterface, 120u);
    CHECK_EQ(kSharedDrmKey, 124u);
    CHECK_EQ(kSharedSq + 3u <= kSharedSmuInterface, true);

    // Derived constants that have no single Linux line to compare against.
    CHECK_EQ(kPgfsmConfigNoPg, 0x15155555u);
    CHECK_EQ(kPgfsmConfigPg, 0x2A2AAAAAu);
    CHECK_EQ(kPgfsmConfigNoPg & ~kPgfsmStatusMask, 0u);
    CHECK_EQ(kPgfsmConfigPg & ~kPgfsmStatusMask, 0u);
    CHECK_EQ(kMpcSetMuxA, 0x040C2040u);
    CHECK_EQ(kMpcSetMuxB, 0x040C2040u);
    CHECK_EQ(kMpcSetMux, 0x88u);
    CHECK_EQ(kPowerStatusKeep, 0xFFFFFEFCu);
    CHECK_EQ(kPowerStatusPgOn, 0x102u);
    CHECK_EQ(kUvdStatusVcpuRdy, 1u << 1);
    CHECK_EQ(kPspFwTypeVcn, 13u);
    CHECK_EQ(kPpsmcPowerUpVcn, 7u);
    CHECK_EQ(kPpsmcPowerDownVcn, 6u);
    CHECK_EQ(kCmdEnd, 1u);
    CHECK_EQ(ringCommitAfter(0, 1), 64u);
    CHECK_EQ(ringCommitAfter(64, kFenceDwords), 128u);
    CHECK_EQ(ringCommitAfter(kRingDw - 64, kFenceDwords), kRingDw);
    CHECK_EQ(ringSlot(ringCommitAfter(kRingDw - 64, kFenceDwords)), 0u);

    // Fence WB slot: dword-aligned, inside the BO and clear of the ring metadata.
    CHECK_EQ(kWbOff % 8u, 0u);
    CHECK(kWbOff >= kMetaOff + kRbMetadataBytes);
    CHECK(kWbOff + kWbSize <= kBoSize);
    CHECK_EQ(kWbSize, 4u);

    // FENCE <lo> <hi> <seq> END, polled without an interrupt trap.
    uint32_t dw[kFenceDwords];
    const uint64_t mc = 0x000000800C8B0000ull;
    buildFencePacket(dw, mc, 0x1234u);
    CHECK_EQ(kFenceDwords, 5u);

    const uint64_t tmr = 0x8008000000ull, tmrBytes = 64ull << 20;
    const uint32_t cache = firmwareCacheBytes(403600u);
    CHECK_EQ(cache, 0x63000u);
    CHECK(firmwareFitsTmr(tmr, cache, tmr, tmrBytes));
    CHECK(firmwareFitsTmr(tmr + tmrBytes - cache, cache, tmr, tmrBytes));
    CHECK(!firmwareFitsTmr(tmr + tmrBytes - cache + 1, cache, tmr, tmrBytes));
    CHECK(!firmwareFitsTmr(tmr - 1, cache, tmr, tmrBytes));
    CHECK(!firmwareFitsTmr(tmr, cache, tmr, 0));
    CHECK(!firmwareFitsTmr(UINT64_MAX, cache, tmr, tmrBytes));
    CHECK_EQ(dw[0], 3u);
    CHECK_EQ(dw[1], 0x0C8B0000u);
    CHECK_EQ(dw[2], 0x80u);
    CHECK_EQ(dw[3], 0x1234u);
    CHECK_EQ(dw[4], 1u);
}

static void testFirmwareHeader(const std::vector<unsigned char> &fw) {
    if (fw.size() < sizeof(FwHeader)) {
        std::printf("SKIP firmware test: file shorter than a header\n");
        return;
    }
    const FwHeader h = loadHeader(fw);
    CHECK_EQ(h.sizeBytes, fw.size());
    // The dword view vcnctl parses: [5] ucode_size_bytes, [6] ucode_array_offset_bytes, [7] crc32.
    uint32_t d[8];
    std::memcpy(d, fw.data(), sizeof(d));
    CHECK_EQ(d[5], 403600u);
    CHECK_EQ(d[6], 256u);
    CHECK_EQ(d[7], h.crc32);
    CHECK_EQ(h.headerSizeBytes, 32u);
    CHECK_EQ(h.headerVersionMajor, 1u);
    CHECK_EQ(h.headerVersionMinor, 0u);
    CHECK_EQ(h.ipVersionMajor, 4u);
    CHECK_EQ(h.ipVersionMinor, 0u);
    CHECK_EQ(h.ucodeArrayOffsetBytes, 256u);
    CHECK_EQ(h.ucodeSizeBytes, 403600u);
    CHECK_EQ((uint64_t)h.ucodeArrayOffsetBytes + h.ucodeSizeBytes, fw.size());

    uint32_t off = 0, size = 0;
    CHECK(fwHeaderOk(h, (uint32_t)fw.size(), &off, &size));
    CHECK_EQ(off, 256u);
    CHECK_EQ(size, 403600u);

    // Every rejection path the kernel relies on.
    FwHeader bad = h;
    bad.ipVersionMajor = 5;                 CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ipVersionMinor = 1;        CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.headerVersionMajor = 2;    CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.headerSizeBytes = 0;       CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.sizeBytes--;              CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ucodeArrayOffsetBytes = 2; CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ucodeArrayOffsetBytes = 257; CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ucodeSizeBytes = 0;        CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ucodeSizeBytes = 17u << 20; CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    bad = h; bad.ucodeSizeBytes = (uint32_t)fw.size();  CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    // 64-bit bounds check: must not wrap and accept an ucode array that ends past the buffer.
    bad = h; bad.ucodeArrayOffsetBytes = 0xFFFFFFF0u; bad.ucodeSizeBytes = 0x100;
    CHECK(!fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    // Exactly fits: offset + size == fileLen is accepted.
    bad = h; bad.ucodeArrayOffsetBytes = (uint32_t)fw.size() - 4u; bad.ucodeSizeBytes = 4;
    CHECK(fwHeaderOk(bad, (uint32_t)fw.size(), &off, &size));
    CHECK_EQ(off, (uint32_t)fw.size() - 4u);
    CHECK_EQ(size, 4u);
}

int main(int argc, char **argv) {
    testLayout();
    const char *path = argc > 1 ? argv[1] : "/Users/Shared/gpu/fw/vcn_4_0_0.bin";
    bool ok = false;
    std::vector<unsigned char> fw = readFile(path, ok);
    if (!ok) {
        std::printf("SKIP firmware test: cannot read %s\n", path);
    } else {
        testFirmwareHeader(fw);
    }
    if (gFailures) {
        std::printf("VCN host test: %d FAILURE(S)\n", gFailures);
        return 1;
    }
    std::printf("VCN host test: OK (layout, derived constants, firmware header)\n");
    return 0;
}
