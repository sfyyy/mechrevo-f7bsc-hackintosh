// Host test for the pure parts of the VCN bring-up: the firmware-header check, the VRAM layout arithmetic and
// the constants that are derived rather than copied. Built and run by tests/vcn_selftest.sh; it never touches
// hardware.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../PhxVcn.h"
#include "../PhxVcnDecode.h"
#include "../PhxVcnDecodeTestVector.h"
#include "../PhxVcnEncode.h"
#include "../PhxVcnEncodeFrame.h"

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
    CHECK_EQ(kPgfsmConfigPg, 0x2A2A9AA5u);
    CHECK_EQ(kPgfsmConfigGate, 0x2A2AAAAAu);
    {   // rebuild the PGFSM words from the UVD_PGFSM_CONFIG field shifts (vcn_4_0_0_sh_mask.h); 1 = on, 2 = off
        const uint32_t sh[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 26, 28}, on3[] = {0, 2, 12};
        uint32_t pg = 0, nopg = 0, gate = 0, pgOnStatus = 0;
        for (uint32_t v : sh) {
            bool m = false;
            for (uint32_t o : on3) m |= o == v;
            pg |= (m ? 1u : 2u) << v; nopg |= 1u << v; gate |= 2u << v; pgOnStatus |= (m ? 0u : 2u) << v;
        }
        CHECK_EQ(kPgfsmConfigPg, pg); CHECK_EQ(kPgfsmConfigNoPg, nopg); CHECK_EQ(kPgfsmConfigGate, gate);
        CHECK_EQ(kPgfsmGated, gate);
        CHECK_EQ(kPgfsmPoweredPg, pgOnStatus);   // status mirrors config: 0 = on, 2 = off
    }
    CHECK_EQ(kMpcSetMuxA, 0x040C2040u);
    CHECK_EQ(kMpcSetMuxB, 0x040C2040u);
    CHECK_EQ(kMpcSetMux, 0x88u);
    CHECK_EQ(kPowerStatusKeep, 0xFFFFFEFCu);
    CHECK_EQ(kPowerStatusMask, 0x3u);
    CHECK_EQ(kPowerStatusPgOn, 0x102u);
    CHECK_EQ(kPowerStatusTilesOff, 1u);
    CHECK_EQ(kPgfsmPoweredNoPg, 0u);
    CHECK_EQ(kPgfsmPoweredPg, 0x2A2A8AA0u);
    // what PowerUpVcn leaves (seen on the device) is "all planes off", never "on"
    CHECK_EQ(0x2AAAAAAAu & kPgfsmStatusMask, kPgfsmGated);
    CHECK((0x2AAAAAAAu & kPgfsmStatusMask) != kPgfsmPoweredPg);
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

static void testDpgList() {
    // Translated independently from the Linux VCN 4.0 register headers and start_dpg_mode.
    // Example MC addresses have distinct halves for firmware, stack, context and shared.
    const uint32_t writes[][2] = {
        {0x00d6, 0x1ff00200}, {0x0021, 0}, {0x01f0, 0x00307308}, {0x0114, 0x10},
        {0x0116, 0x040c2040}, {0x0118, 0x040c2040}, {0x011a, 0x88},
        {0x0184, 0}, {0x0185, 0}, {0x00c0, 0}, {0x00c1, 0},
        {0x01b0, 0}, {0x01b1, 0}, {0x00c2, 0}, {0x00c3, 0x20000},
        {0x01b4, 0x45678000}, {0x01b5, 0x82}, {0x00c4, 0}, {0x00c5, 0x80000},
        {0x0180, 0x56789000}, {0x0181, 0x83}, {0x00d2, 0}, {0x00d3, 0x1000},
        {0xc042, 0x242}, {0x00d6, 0x0ff00200}, {0x01ee, 0x003e0000}, {0x0021, 2},
    };
    for (bool indirect : {false, true}) {
        unsigned int pair = 0;
        const auto check = [&](uint32_t reg, uint32_t value) {
            if (pair < sizeof(writes) / sizeof(writes[0])) {
                uint32_t expected = writes[pair][1];
                if (!indirect) {
                    switch (pair) {
                    case 7: expected = 0x23456000; break;
                    case 8: expected = 0x80; break;
                    case 10: expected = 0x63000; break;
                    case 11: expected = 0x34567000; break;
                    case 12: expected = 0x81; break;
                    }
                }
                CHECK_EQ(dpgModeOffset(reg), writes[pair][0]);
                CHECK_EQ(value, expected);
            }
            ++pair;
        };
        writeDpgSetup(check, indirect, 0x8023456000ull, 0x63000, 0x8134567000ull,
                      0x8245678000ull, 0x8356789000ull, 0x242);
        CHECK_EQ(pair, 24u);
        writeDpgRelease(check);
        CHECK_EQ(pair * 8u, 216u);
    }
}

static void testSessionRingPacket() {
    uint32_t packet[10] = {};
    buildIbFencePacket(packet, 0x800c8b3000ull, 72, 0x800c8b0000ull, 17);
    const uint32_t expected[] = {2, 0, 0x0c8b3000, 0x80, 72, 3, 0x0c8b0000, 0x80, 17, 1};
    CHECK(std::memcmp(packet, expected, sizeof(packet)) == 0);
}

static void testDecodeSession() {
    const uint32_t create[] = {40, 56, 1, 0, 1, 0, 1, 40, 16, 0, 7, 0, 1920, 1088};
    const uint32_t destroy[] = {40, 24, 0, 2, 1, 0};
    const uint32_t sentinel = 0xa5a5a5a5;
    uint32_t words[44];
    for (bool close : {false, true}) {
        for (uint32_t &word : words) word = sentinel;
        const size_t bytes = close ? sizeof(destroy) : sizeof(create);
        CHECK_EQ(buildDecodeSessionMsg(words, close), bytes / sizeof(uint32_t));
        CHECK(std::memcmp(words, close ? destroy : create, bytes) == 0);
        CHECK_EQ(words[bytes / sizeof(uint32_t)], sentinel);
    }
    // Firmware reads address high then low in the decode buffer, opposite to the outer IB packet.
    const uint32_t prefix[] = {16, 0x30000002, 0x3c8b51be, 39, 16, 0x30000001, 3, 156,
                               140, 1, 1, 0x80, 0x0c8b5000};
    for (uint32_t &word : words) word = sentinel;
    CHECK_EQ(buildDecodeSessionIb(words, 0x800c8b5000ull), 43u);
    CHECK(std::memcmp(words, prefix, sizeof(prefix)) == 0);
    for (unsigned i = sizeof(prefix) / sizeof(prefix[0]); i < 43; ++i)
        CHECK_EQ(words[i], 0u);
    CHECK_EQ(words[43], sentinel);
}

static void testDecodeFrame() {
    const DecodeFrameBuffers buffers = {
        0x8100100000ull, 0x8200200000ull, 0x8300300000ull, 0x8400400000ull,
        0x8500500000ull, 0x8600600000ull, 0x8700700000ull, 0x8800800000ull
    };
    uint32_t words[44];
    for (uint32_t &word : words) word = 0xdeadcafe;
    CHECK_EQ(buildDecodeFrameIb(words, buffers), 43u);
    const uint32_t expected[43] = {
        16, 0x30000002, 0x32500f80, 39, 16, 0x30000001, 3, 156, 140, 1, 0x00100a1f,
        0x81, 0x100000, 0x82, 0x200000, 0x83, 0x300000, 0x84, 0x400000,
        0x85, 0x500000, 0x86, 0x600000, 0x87, 0x700000, 0, 0, 0, 0, 0, 0, 0x88, 0x800000,
    };
    CHECK(std::memcmp(words, expected, sizeof(expected)) == 0);
    CHECK_EQ(words[43], 0xdeadcafeu);
    CHECK_EQ(buildDecodeSessionIb(words, buffers.message, buffers.sessionContext), 43u);
    CHECK_EQ(words[10], 0x100001u);
    CHECK_EQ(words[17], 0x84u);
    CHECK_EQ(words[18], 0x400000u);
    CHECK_EQ(words[43], 0xdeadcafeu);

    uint32_t header[59];
    static_assert(sizeof(header) == sizeof(kDecodeH264Message), "message header size");
    std::memcpy(header, kDecodeH264Message, sizeof(header));
    CHECK_EQ(header[4], kDecodeFrameHandle);
    CHECK_EQ(header[11], sizeof(kDecodeH264Message));
    CHECK_EQ(header[16], kDecodeFrameWidth);
    CHECK_EQ(header[17], kDecodeFrameHeight);
    CHECK_EQ(header[18], sizeof(kDecodeH264Bitstream));
    CHECK_EQ(header[24], kDecodeSessionBytes);
    CHECK_EQ(kDecodeTargetBytes, 1024u * 480u * 3u / 2u);
    CHECK_EQ(kDecodeOutputByteSum, 0x020345d8u);
    CHECK_EQ(sizeof(kDecodeH264ScalingTable), 224u);
    CHECK_EQ(sizeof(kDecodeH264Parameters), 3344u);
    CHECK_EQ(sizeof(kDecodeH264Feedback), 44u);
}

static void testEncodeSession() {
    // Golden create and destroy IBs from the layout used by amdgpu_vcn_enc_ring_test_ib.
    // Use different high and low address words so swapping the words fails this check.
    for (bool close : {false, true}) {
        const uint32_t expected[] = {
            16, 0x30000002, close ? 0x448c012du : 0x448c012cu, 17,
            16, 0x30000001, 2, 68,
            24, 1, 1, 0x80, 0x0c8c0000, 0,
            20, 2, 28, 0, 0,
            8, close ? 0x08000002u : 0x08000001u,
        };
        uint32_t words[22];
        for (uint32_t &word : words) word = 0xdeadbeef;
        CHECK_EQ(buildEncodeSessionIb(words, 0x800c8c0000ull, close), 21u);
        CHECK(std::memcmp(words, expected, sizeof(expected)) == 0);
        CHECK_EQ(words[21], 0xdeadbeefu);
    }
}

static void testEncodeFramePackets() {
    // Generated independently with libdrm's VCN 4 constructors for these addresses,
    // changing only its two bitstream capacity fields to match our allocation.
    constexpr uint64_t ctx = 0x8101234000ull;
    constexpr uint32_t count[] = {99, 449, 21};
    constexpr uint32_t checksums[] = {0x35b57495, 0xb95843c4, 0x3224413a};
    constexpr uint64_t hashes[] = {0xa22ad20065090ca2ull, 0xaa63a93f030b8848ull, 0x7a09168cf866b3a4ull};
    for (unsigned phase = 0; phase < 3; ++phase) {
        uint32_t words[kEncodeFrameIbDw + 1];
        for (uint32_t &v : words) v = 0xdeadbeefu;
        const unsigned n = phase == 1 ? buildEncodeFrameIb(words, ctx, 0x8205678000ull,
            0x83ffffe000ull, 0x840def0000ull, 0x8501230000ull, 2) :
            buildEncodeFrameSessionIb(words, ctx, phase + 1, phase == 2);
        CHECK_EQ(n, count[phase]);
        if (n != count[phase]) continue;
        CHECK_EQ(words[n], 0xdeadbeefu);
        CHECK_EQ(words[2], checksums[phase]);
        CHECK_EQ(words[3], n - 4u);
        CHECK_EQ(words[7], 4u * (n - 4u));
        CHECK_EQ(words[16], 4u * (n - 14u));
        CHECK_EQ(words[17], phase + 1u);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (unsigned i = 0; i < n; ++i) hash = (hash ^ words[i]) * 0x100000001b3ull;
        CHECK_EQ(hash, hashes[phase]); // order-sensitive, covers every field and padding dword
        if (phase != 1) continue;
        const uint32_t packages[][2] = {
            {6, 1}, {5, 2}, {9, 0xa}, {6, 0xa}, {50, 0xb}, {13, 0xf}, {20, 0x00200003},
            {294, 0x11}, {7, 0x12}, {7, 0x15}, {5, 0x10}, {9, 0xc}, {6, 0xd},
            {2, 0x01000006}, {2, 0x01000003},
        };
        unsigned at = 8;
        for (const auto &pkg : packages) {
            CHECK_EQ(words[at], pkg[0] * 4u);
            CHECK_EQ(words[at + 1], pkg[1]);
            at += pkg[0];
        }
        CHECK_EQ(at, n);
        CHECK_EQ(words[90], 0x84u); // luma MC crosses 4 GiB when adding the UV offset
        CHECK_EQ(words[91], 0x6000u);
        CHECK_EQ(words[92], kEncodeFramePitch);
        CHECK_EQ(words[93], kEncodeFramePitch / 2u);
    }
}

static void testEncodeFrameBuffers() {
    std::vector<uint8_t> pixels(kEncodeFrameInputBytes + 1, 0xfe);
    CHECK(!initializeEncodeFrame(pixels.data(), kEncodeFrameInputBytes - 1u));
    CHECK_EQ(pixels[0], 0xfeu);
    CHECK(initializeEncodeFrame(pixels.data(), kEncodeFrameInputBytes));
    CHECK_EQ(pixels[0], 48u);
    CHECK_EQ(pixels[kEncodeFrameWidth - 1], 200u);
    CHECK_EQ(pixels[kEncodeFrameWidth], 16u);
    CHECK_EQ(pixels[kEncodeFramePitch * (kEncodeFrameHeight - 1)], 200u);
    CHECK_EQ(pixels[kEncodeFrameLumaBytes], 128u);
    CHECK_EQ(pixels[kEncodeFrameInputBytes - 1], 128u);
    CHECK_EQ(pixels[kEncodeFrameInputBytes], 0xfeu);

    std::vector<uint8_t> input(PHXN_VCN_NV12_BYTES);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i * 73u + i / kEncodeFrameWidth);
    CHECK(!copyPackedNv12ToEncodeFrame(nullptr, kEncodeFrameInputBytes, input.data(), input.size()));
    CHECK(!copyPackedNv12ToEncodeFrame(pixels.data(), kEncodeFrameInputBytes, nullptr, input.size()));
    CHECK(!copyPackedNv12ToEncodeFrame(pixels.data(), kEncodeFrameInputBytes - 1u, input.data(), input.size()));
    CHECK(!copyPackedNv12ToEncodeFrame(pixels.data(), kEncodeFrameInputBytes, input.data(), input.size() - 1u));
    CHECK(!copyPackedNv12ToEncodeFrame(pixels.data(), kEncodeFrameInputBytes, input.data(), input.size() + 1u));
    CHECK_EQ(pixels[0], 48u);
    CHECK(copyPackedNv12ToEncodeFrame(pixels.data(), kEncodeFrameInputBytes, input.data(), input.size()));
    for (uint32_t y = 0; y < kEncodeFrameHeight * 3u / 2u; ++y) {
        const uint8_t *row = pixels.data() + y * kEncodeFramePitch;
        CHECK(std::memcmp(row, input.data() + y * kEncodeFrameWidth, kEncodeFrameWidth) == 0);
        for (uint32_t x = kEncodeFrameWidth; x < kEncodeFramePitch; ++x)
            CHECK_EQ(row[x], y < kEncodeFrameHeight ? 16u : 128u);
    }
    CHECK_EQ(pixels[kEncodeFrameInputBytes], 0xfeu);

    uint8_t sample[] = {0,0,0,1,0x67,100,0,11,0x80, 0,0,1,0x68,0xce,0x80, 0,0,1,0x65,0xb8};
    uint8_t feedback[40] = {};
    feedback[4] = 1; // RENCODE feedback available
    feedback[24] = sizeof(sample);
    uint32_t bytes = 99;
    CHECK(encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    CHECK_EQ(bytes, sizeof(sample));
    feedback[4] = 0;
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    CHECK_EQ(bytes, 0u);
    feedback[4] = 1;
    feedback[32] = 5; // Phoenix build 62 capture: SPS still starts at byte zero
    CHECK(encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    CHECK_EQ(bytes, sizeof(sample));
    CHECK(encodedH264IdrOk(feedback, 28, sample, sizeof(sample), &bytes));
    CHECK(!encodedH264IdrOk(feedback, 27, sample, sizeof(sample), &bytes));
    CHECK_EQ(bytes, 0u);
    feedback[24] = 0;
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    feedback[24] = sizeof(sample);
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample) - 1, &bytes));
    feedback[24] = sizeof(sample) - 1; // IDR header, no RBSP
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    feedback[24] = sizeof(sample);
    sample[18] = 0x41; // non-IDR picture
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    sample[18] = 0x65;
    sample[4] = 0x68; // PPS before SPS
    CHECK(!encodedH264IdrOk(feedback, sizeof(feedback), sample, sizeof(sample), &bytes));
    CHECK_EQ(bytes, 0u);
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

static void testSizedEncode() {
    EncodeGeometry g;
    CHECK(!sizedEncodeGeometry(961, 540, g));
    CHECK(!sizedEncodeGeometry(960, 541, g));
    CHECK(!sizedEncodeGeometry(UINT32_MAX, 128, g));
    CHECK(!sizedEncodeGeometry(2562, 1440, g));
    CHECK(!sizedEncodeGeometry(1920, 1920, g));
    CHECK(!sizedEncodeGeometry(120, 144, g));
    const uint32_t sizes[][2] = {{128,128}, {160,128}, {640,480}, {854,480}, {960,540},
        {962,542}, {1280,720}, {1920,1080}, {2048,1152}, {2560,1440}, {1440,2560}};
    for (const auto &size : sizes) {
        CHECK(sizedEncodeGeometry(size[0], size[1], g));
        CHECK_EQ(g.codedWidth, (size[0] + 15u) & ~15u);
        CHECK_EQ(g.codedHeight, (size[1] + 15u) & ~15u);
        CHECK_EQ(g.pitch % 256, 0u);
        CHECK_EQ(g.inputOff, 3u * g.recLumaBytes);
        CHECK(g.bitstreamOff >= g.inputOff + g.lumaBytes * 3u / 2u);
        CHECK(g.bitstreamOff + g.bitstreamBytes <= g.feedbackOff);
        CHECK(g.scratchBytes <= kEncodeScratchMax);
        uint32_t words[kEncodeFrameMaxIbDw + 1]{};
        words[kEncodeFrameMaxIbDw] = 0xdeadbeef;
        buildEncodeFrameSessionIb(words, 0x8100000000u, 1, false, g);
        CHECK_EQ(words[24], g.codedWidth);
        CHECK_EQ(words[25], g.codedHeight);
        CHECK_EQ(words[26], g.codedWidth - g.width);
        CHECK_EQ(words[27], g.codedHeight - g.height);
        uint32_t n = buildEncodeFrameIb(words, 0x8100000000u, 0x8200000000u, 0x8300000000u,
                                        0x8400000000u, 0x8500000000u, 2, g);
        CHECK(n && n <= kEncodeFrameMaxIbDw);
        CHECK_EQ(words[kEncodeFrameMaxIbDw], 0xdeadbeefu);
        uint32_t checksum = 0;
        for (uint32_t i = 4; i < n; ++i) checksum += words[i];
        CHECK_EQ(checksum, words[2]);
    }
    CHECK(sizedEncodeGeometry(962, 542, g));
    std::vector<uint8_t> packed(phxnVcnPackedBytes(g.width, g.height), 97u);
    std::fill(packed.begin() + g.width * g.height, packed.end(), 155u);
    std::vector<uint8_t> dst(g.lumaBytes * 3u / 2u + 1u, 0xabu);
    CHECK(copyPackedNv12ToEncodeFrame(dst.data(), dst.size()-1u, packed.data(), packed.size(), g));
    CHECK_EQ(dst[g.width - 1u], 97u);
    CHECK_EQ(dst[g.width], 16u);
    CHECK_EQ(dst[g.height * g.pitch], 16u);
    CHECK_EQ(dst[g.lumaBytes], 155u);
    CHECK_EQ(dst[g.lumaBytes + g.width], 128u);
    CHECK_EQ(dst[g.lumaBytes + (g.height / 2u) * g.pitch], 128u);
    CHECK_EQ(dst.back(), 0xabu);
}

int main(int argc, char **argv) {
    testLayout();
    testDpgList();
    testSessionRingPacket();
    testDecodeSession();
    testDecodeFrame();
    testEncodeSession();
    testEncodeFramePackets();
    testEncodeFrameBuffers();
    testSizedEncode();
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
    std::printf("VCN host test: OK (layout, DPG, firmware, session IBs, H.264 frame IBs and payload checks)\n");
    return 0;
}
