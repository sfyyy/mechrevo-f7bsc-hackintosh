// Host test for the pure parts of the VCN bring-up: the firmware-header check, the VRAM layout arithmetic and
// the constants that are derived rather than copied. Built and run by tests/vcn_selftest.sh; it never touches
// hardware.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../PhxVcn.h"
#include "../PhxVcnAperture.h"
#include "../PhxVcnAvcStream.h"
#include "../PhxVcnDecode.h"
#include "../PhxVcnDecodeTestVector.h"
#include "../PhxVcnEncode.h"
#include "../PhxVcnEncodeFrame.h"
#include "../PhxVcnWait.h"

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

static void testApertureTransfers() {
    alignas(8) uint8_t device[80], ram[80], reference[80], result[80];
    for (size_t i = 0; i < sizeof(ram); ++i) ram[i] = static_cast<uint8_t>(i * 37);
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (unsigned n = 0; n <= 64; ++n) {
            std::memset(device, 0x42, sizeof(device));
            std::memset(reference, 0x42, sizeof(reference));
            std::memcpy(reference + 8, ram + offset, n);
            writeAperture(device + 8, ram + offset, n);
            CHECK(std::memcmp(device, reference, sizeof(device)) == 0);

            std::memset(result, 0x99, sizeof(result));
            std::memset(reference, 0x99, sizeof(reference));
            std::memcpy(reference + offset, ram + offset, n);
            readApertureRow(result + offset, device + 8, n);
            CHECK(std::memcmp(result, reference, sizeof(result)) == 0);

            std::memcpy(reference, device, sizeof(reference));
            std::memset(reference + offset, 0, n);
            clearAperture(device + offset, n);
            CHECK(std::memcmp(device, reference, sizeof(device)) == 0);
        }
    }
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
    uint8_t idr[PHX_VIDEO_DECODE_INPUT_BYTES];
    std::memcpy(idr, kDecodeH264Bitstream, sizeof(idr));
    CHECK(isDecodeReferenceIDR(idr, sizeof(idr)));
    CHECK(!isDecodeReferenceIDR(nullptr, sizeof(idr)));
    CHECK(!isDecodeReferenceIDR(idr, sizeof(idr) - 1));
    CHECK(!isDecodeReferenceIDR(kDecodeH264Bitstream, sizeof(kDecodeH264Bitstream)));
    idr[sizeof(idr) - 1] ^= 1;
    CHECK(!isDecodeReferenceIDR(idr, sizeof(idr)));
}

static void testAvcStream() {
    AvcStreamSequence sequence{1920, 1088, 1920, 1080, 100, 40, 0, 0, 0, 4, true};
    AvcStreamGeometry geometry;
    CHECK(avcStreamGeometry(sequence, geometry));
    CHECK_EQ(geometry.pitch, 2048u);
    CHECK_EQ(geometry.lumaBytes, geometry.pitch * 1088u);
    CHECK_EQ(geometry.targetBytes, geometry.lumaBytes * 3u / 2u);
    CHECK_EQ(geometry.dpbPitch, 1920u);
    CHECK_EQ(geometry.dpbChromaPitch, 960u);
    CHECK_EQ(geometry.dpbHeight, 1088u);
    CHECK_EQ(geometry.dpbBytes, 5u * (1920u * 1088u * 3u / 2u));
    CHECK_EQ(geometry.contextBytes, 5u * (120u * 68u * 192u));
    // 1080p with four references does not fit DPB + context in one 16 MiB context slot;
    // its DPB moves to a bank. (One or two references stay in the slot.)
    CHECK(geometry.separateDpb);
    CHECK_EQ(geometry.dpbOffset, 0u);
    CHECK_EQ(geometry.contextOffset, kDecodeSessionBytes);
    CHECK(geometry.persistentBytes >= geometry.contextOffset + geometry.contextBytes);
    CHECK_EQ(geometry.targetOffset, geometry.bitstreamOffset + kAvcStreamMaxBitstreamBytes);
    CHECK(geometry.stagingBytes >= geometry.targetOffset + geometry.targetBytes);
    CHECK_EQ(geometry.stagingBytes, 4403200u);
    CHECK(geometry.persistentBytes <= kAvcStreamSlotBytes);
    CHECK(geometry.stagingBytes <= kEncodeScratchMax);
    CHECK_EQ(kAvcStreamScratchOffset, kDecodeFrameScratchOffset + kEncodeScratchMax);
    CHECK_EQ(kAvcStreamScratchOffset + kAvcStreamScratchLimit, 256u * 1024u * 1024u);
    const uint64_t fb = 0x8100000000ull;
    const uint64_t staging = fb + kDecodeFrameScratchOffset;
    {
        // The first session placed in an empty pool lands at the pool start.
        const AvcRange none[kAvcStreamSlotCount]{};
        uint32_t ctxOff = 0, dpbOff = 0;
        CHECK(phx_vcn::avcPoolAlloc(kAvcContextBankOffset, kAvcContextPoolBytes, none, kAvcStreamSlotCount,
                                    geometry.persistentBytes, ctxOff));
        CHECK(phx_vcn::avcPoolAlloc(kAvcDpbBankOffset, kAvcDpbPoolBytes, none, kAvcStreamSlotCount,
                                    geometry.dpbBytes, dpbOff));
        const uint64_t slot = fb + ctxOff;
        const auto buffers = avcStreamBuffers(staging, slot, geometry, fb + dpbOff);
        CHECK_EQ(ctxOff, kAvcContextBankOffset);
        CHECK_EQ(buffers.sessionContext, slot);
        CHECK_EQ(buffers.message, staging);
        CHECK_EQ(buffers.feedback, staging + kAvcStreamAlignment);
        CHECK_EQ(buffers.scalingTable, staging + 2u * kAvcStreamAlignment);
        CHECK_EQ(buffers.bitstream, staging + 3u * kAvcStreamAlignment);
        CHECK_EQ(buffers.target, staging + geometry.targetOffset);
        CHECK_EQ(buffers.dpb, fb + dpbOff);
        CHECK_EQ(buffers.context, slot + geometry.contextOffset);
        CHECK(buffers.context + geometry.contextBytes <= slot + geometry.persistentBytes);
    }
    // Pools: the context pool is the 48 MiB window build 79 used (nothing above 128 MiB), the DPB pool is
    // GPU-only, above BAR0 and clear of PhxAMDFB's second scan-out surface (512 MiB + up to 64 MiB).
    CHECK(kAvcContextBankOffset + kAvcContextPoolBytes <= 128u * 1024u * 1024u);
    CHECK(kAvcContextBankOffset + kAvcContextPoolBytes <= kBoOffset);
    CHECK(kAvcDpbBankOffset >= 576u * 1024u * 1024u);
    CHECK(kAvcDpbBankOffset + kAvcDpbPoolBytes <= 2048u * 1024u * 1024u);
    {
        // Allocator: first fit, aligned, no overlap, reuse of freed holes, exhaustion.
        const uint32_t pool = 1u << 20, size = 8u << 20;
        AvcRange used[kAvcStreamSlotCount]{};
        uint32_t off = 0;
        CHECK(phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 3u << 20, off));
        CHECK_EQ(off, pool);
        used[0] = {off, 3u << 20};
        CHECK(phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 1u, off));
        CHECK_EQ(off, pool + (3u << 20));
        used[1] = {off, 1u};
        CHECK(phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 1u << 20, off));
        CHECK_EQ(off % kAvcPoolAlignment, 0u);
        CHECK_EQ(off, pool + (3u << 20) + kAvcPoolAlignment);
        used[2] = {off, 1u << 20};
        used[0] = {};   // first session closed: its hole is reused by something that fits
        CHECK(phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 2u << 20, off));
        CHECK_EQ(off, pool);
        CHECK(!phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 5u << 20, off));   // no hole big enough
        CHECK(!phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, size + 1u, off));
        CHECK(!phx_vcn::avcPoolAlloc(pool, size, used, kAvcStreamSlotCount, 0u, off));
    }

    AvcStreamPicture picture{};
    picture.idr = true;
    picture.entropyCoding = picture.transform8x8 = picture.deblockingControlPresent = true;
    picture.picInitQpMinus26 = -4;
    picture.chromaQpIndexOffset = -2;
    picture.secondChromaQpIndexOffset = 3;
    uint32_t words[kAvcStreamMessageDw];
    uint8_t scaling[kAvcStreamScalingBytes];
    constexpr uint32_t handle = 0xa4020001;
    constexpr uint32_t serial = 1;
    CHECK(buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial, 1025));
    const uint32_t index[14] = {40, 1368, 2, 1, handle, serial, 2, 56, 188, 0, 6, 244, 1124, 0};
    CHECK(std::memcmp(words, index, sizeof(index)) == 0);
    const uint32_t *decode = words + words[7] / sizeof(uint32_t);
    CHECK_EQ(decode[0], 7u);
    CHECK_EQ(decode[2], sequence.codedWidth);
    CHECK_EQ(decode[3], sequence.codedHeight);
    CHECK_EQ(decode[4], 1152u);
    CHECK_EQ(decode[5], geometry.dpbBytes);
    CHECK_EQ(decode[6], geometry.targetBytes);
    CHECK_EQ(decode[9], geometry.contextBytes);
    CHECK_EQ(decode[10], kDecodeSessionBytes);
    CHECK_EQ(decode[18], geometry.dpbPitch);
    CHECK_EQ(decode[19], geometry.dpbHeight);
    CHECK_EQ(decode[25], geometry.pitch);
    CHECK_EQ(decode[26], geometry.pitch / 2u);
    CHECK_EQ(decode[36], geometry.lumaBytes);
    CHECK_EQ(decode[37], geometry.lumaBytes);
    CHECK_EQ(decode[41], geometry.dpbChromaPitch);
    const uint32_t *codec = words + words[11] / sizeof(uint32_t);
    CHECK_EQ(codec[0], 2u);
    CHECK_EQ(codec[1], 40u);
    CHECK_EQ(codec[2], 0x85u);
    CHECK_EQ(codec[3], 0x109u);
    CHECK_EQ(codec[4], 1u);
    CHECK_EQ(codec[5], 0x40000u);
    CHECK_EQ(codec[6], 0x03fe00fcu);
    CHECK(std::memcmp(codec + 9, scaling, sizeof(scaling)) == 0);
    CHECK_EQ(scaling[0], 16u);
    CHECK_EQ(scaling[sizeof(scaling) - 1u], 16u);
    CHECK_EQ(codec[65], 0u);
    CHECK_EQ(codec[116], 0u);
    CHECK_EQ(codec[117], 0u);
    for (size_t i = 118; i < 122; ++i) CHECK_EQ(codec[i], UINT32_MAX);
    CHECK_EQ(codec[280], 0u);

    picture.idr = false;
    picture.frameNum = 2;
    picture.referenceCount = 1;
    picture.references[0] = {1, 2, 2, 1};
    picture.topPoc = picture.bottomPoc = 4;
    CHECK(buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial + 1, kAvcStreamMaxBitstreamBytes));
    CHECK_EQ(decode[4], kAvcStreamMaxBitstreamBytes);
    CHECK_EQ(codec[65], 2u);
    CHECK_EQ(codec[66], 1u);
    CHECK_EQ(codec[82], 4u);
    CHECK_EQ(codec[83], 4u);
    CHECK_EQ(codec[84], 2u);
    CHECK_EQ(codec[85], 2u);
    CHECK_EQ(codec[116], 0u);
    CHECK_EQ(codec[117], 1u);
    CHECK_EQ(codec[118], 0xffffff01u);
    CHECK_EQ(codec[280], 3u);
    CHECK(!buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial, kAvcStreamMaxBitstreamBytes + 1));
    CHECK(!buildAvcStreamMessage(words, scaling, sequence, picture, 0, serial, 128));
    CHECK(!buildAvcStreamMessage(words, scaling, sequence, picture, handle, 0, 128));
    picture.references[0].slot = picture.currentSlot;
    CHECK(!buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial, 128));
    picture.references[0].slot = 1;
    sequence.maxNumRefFrames = 0;
    CHECK(!buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial, 128));
    sequence = {1280, 720, 1280, 720, 66, 31, 0, 2, 0, 1, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    sequence.levelIdc = 30;
    CHECK(!avcStreamGeometry(sequence, geometry));
    sequence = {864, 480, 864, 480, 100, 30, 0, 0, 3, 1, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    CHECK_EQ(geometry.targetBytes, kDecodeTargetBytes);
    CHECK_EQ(geometry.dpbPitch, 864u);
    CHECK_EQ(geometry.dpbChromaPitch, 448u);
    CHECK_EQ(geometry.dpbHeight, 512u);
    CHECK_EQ(geometry.dpbBytes, 1343488u);
    CHECK(geometry.separateDpb);
    CHECK_EQ(geometry.persistentBytes, 0xb8000u);   // session + context only; the 1.3 MiB DPB lives in the DPB pool
    CHECK_EQ(geometry.stagingBytes, 1798144u);
    CHECK(buildAvcStreamMessage(words, scaling, sequence, picture, handle, serial, 128));
    CHECK_EQ(decode[18], 864u);
    CHECK_EQ(decode[19], 512u);
    CHECK_EQ(decode[41], 448u);
    sequence.maxNumRefFrames = 2;
    CHECK(avcStreamGeometry(sequence, geometry));
    // The DPB goes to the GPU-only pool, so only the firmware session and the codec context use the context pool.
    CHECK(geometry.separateDpb);
    CHECK_EQ(geometry.persistentBytes, avcStreamAlign(kDecodeSessionBytes + geometry.contextBytes, kAvcStreamAlignment));

    sequence = {1920, 1088, 1920, 1080, 100, 40, 0, 0, 2, 4, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    CHECK(geometry.separateDpb);
    {
        // Capacity: how many 1080p streams the pools admit, and that many small ones fit.
        AvcRange ctx[kAvcStreamSlotCount]{}, dpb[kAvcStreamSlotCount]{};
        uint32_t placed = 0, off = 0;
        for (; placed < kAvcStreamSlotCount; ++placed) {
            if (!phx_vcn::avcPoolAlloc(kAvcContextBankOffset, kAvcContextPoolBytes, ctx, kAvcStreamSlotCount,
                                       geometry.persistentBytes, off)) break;
            ctx[placed] = {off, geometry.persistentBytes};
            CHECK(phx_vcn::avcPoolAlloc(kAvcDpbBankOffset, kAvcDpbPoolBytes, dpb, kAvcStreamSlotCount,
                                        geometry.dpbBytes, off));
            dpb[placed] = {off, geometry.dpbBytes};
        }
        CHECK_EQ(placed, kAvcStreamSlotCount);   // 1080p refs=4 streams: every handle usable
        sequence = {1280, 720, 1280, 720, 100, 40, 0, 0, 2, 4, true};
        CHECK(avcStreamGeometry(sequence, geometry));
        AvcRange small[kAvcStreamSlotCount]{};
        for (uint32_t i = 0; i < kAvcStreamSlotCount; ++i) {   // every handle usable with 720p streams
            CHECK(phx_vcn::avcPoolAlloc(kAvcContextBankOffset, kAvcContextPoolBytes, small, kAvcStreamSlotCount,
                                        geometry.persistentBytes, off));
            small[i] = {off, geometry.persistentBytes};
        }
        sequence = {1920, 1088, 1920, 1080, 100, 40, 0, 0, 2, 4, true};
        CHECK(avcStreamGeometry(sequence, geometry));
    }

    // 1080p High level 5.0 with eight references: B 站 live "high frame rate" streams.
    // The level table stopped at 4.2 and the 8 MiB slot was too small for the 13.4 MiB
    // context, so both a level 5.0 picture and a same-size level 4.2 one were rejected.
    sequence = {1920, 1088, 1920, 1080, 100, 50, 0, 0, 2, 8, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    CHECK(geometry.separateDpb);
    CHECK(geometry.dpbBytes <= kAvcDpbBankBytes);
    CHECK(geometry.persistentBytes <= kAvcStreamSlotBytes);
    CHECK(geometry.contextBytes > 8u * 1024u * 1024u);
    sequence.levelIdc = 42;  // 8160 * 8 > MaxDpbMbs(4.2): the level budget, not ours
    CHECK(!avcStreamGeometry(sequence, geometry));
    sequence.levelIdc = 50;
    CHECK(avcStreamGeometry(sequence, geometry));

    // 720p High level 4.0 with eight references (live streams); previously rejected at four.
    sequence = {1280, 720, 1280, 720, 100, 40, 0, 0, 2, 8, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    CHECK(geometry.dpbBytes <= kAvcDpbBankBytes);
    CHECK(geometry.persistentBytes <= kAvcStreamSlotBytes);
    sequence.maxNumRefFrames = 16;  // 3600 * 16 > MaxDpbMbs(4.0): level limit, not ours
    CHECK(!avcStreamGeometry(sequence, geometry));
    sequence = {640, 368, 640, 360, 100, 40, 2, 0, 2, 16, true};
    CHECK(avcStreamGeometry(sequence, geometry));
    {
        AvcStreamPicture p{};
        p.frameNum = 17; p.currentSlot = 16; p.referenceCount = 16;
        for (uint32_t i = 0; i < 16; ++i) p.references[i] = {1u + i, int32_t(2 * i), int32_t(2 * i), uint8_t(i)};
        CHECK(buildAvcStreamMessage(words, scaling, sequence, p, handle, serial, 128));
        const uint32_t *codec16 = words + (40u + 16u + 188u) / 4u;
        CHECK_EQ(codec16[117], 16u);
        CHECK_EQ(codec16[66 + 15], 16u);
        CHECK_EQ(codec16[280], 0xffffffffu);
    }

    uint32_t feedback[kAvcStreamFeedbackDw];
    initAvcStreamFeedback(feedback);
    CHECK_EQ(feedback[0], 44u);
    CHECK_EQ(feedback[1], 44u);
    CHECK(!avcStreamFeedbackOK(feedback, serial));
    const uint32_t referenceFeedback[kAvcStreamFeedbackDw] = {0x2c, 0x2c, 0, 1, 0, 1, 0, 0, 0, 0, 0};
    CHECK(avcStreamFeedbackOK(referenceFeedback, 1));
    feedback[3] = serial;
    CHECK(avcStreamFeedbackOK(feedback, serial));
    CHECK(!avcStreamFeedbackOK(feedback, serial + 1));
    feedback[4] = 1;
    CHECK(!avcStreamFeedbackOK(feedback, serial));
    feedback[4] = 0;
    feedback[6] = 1;
    CHECK(!avcStreamFeedbackOK(feedback, serial));
    feedback[6] = 0;
    feedback[1] = kAvcStreamAlignment + 1;
    CHECK(!avcStreamFeedbackOK(feedback, serial));
}

static void testAvcStreamReferenceSlots() {
    AvcStreamSequence sequence{1920, 1088, 1920, 1080, 100, 40, 0, 2, 0, 1, true};
    uint32_t words[kAvcStreamMessageDw];
    uint8_t scaling[kAvcStreamScalingBytes];
    unsigned previousSlot = 0, previousFrame = 0;
    for (unsigned i = 0; i < 48; ++i) {
        // New IDR after an even-numbered P, so it must discard the reference
        // in slot 0. Both segments include the 15 -> 0 frame_num transition.
        const unsigned distance = i < 25 ? i : i - 25;
        AvcStreamPicture p{};
        p.idr = distance == 0;
        p.frameNum = distance % 16;
        p.topPoc = p.bottomPoc = static_cast<int32_t>(distance * 2);
        if (!p.idr) {
            p.currentSlot = static_cast<uint8_t>(1u - previousSlot);
            p.referenceCount = 1;
            p.references[0] = {previousFrame, p.topPoc - 2, p.topPoc - 2, static_cast<uint8_t>(previousSlot)};
        }
        CHECK(buildAvcStreamMessage(words, scaling, sequence, p, 1, distance + 1, 512));
        const uint32_t *codec = words + words[11] / sizeof(uint32_t);
        const unsigned currentSlot = p.idr ? 0 : 1u - previousSlot;
        CHECK_EQ(codec[65], p.frameNum);
        CHECK_EQ(codec[66], p.idr ? 0u : previousFrame);
        CHECK_EQ(codec[116], currentSlot);
        CHECK_EQ(codec[117], p.idr ? 0u : 1u);
        const uint8_t *refs = reinterpret_cast<const uint8_t *>(codec + 118);
        CHECK_EQ(refs[0], p.idr ? 0xffu : previousSlot);
        for (unsigned ref = 1; ref < 16; ++ref) CHECK_EQ(refs[ref], 0xffu);
        CHECK_EQ(codec[280], p.idr ? 0u : 3u);
        previousSlot = currentSlot;
        previousFrame = p.frameNum;
    }
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

// An IDR may reuse the firmware session only when nothing the create message
// fixed has changed; a failed frame must force the next IDR to recreate it.
static void testAvcSessionReuseKey() {
    using namespace phx_vcn;
    const AvcSessionKey stored{1920, 1088, 1u << 20, 4u << 20, 8u << 20, 100, 4};
    AvcSessionKey wanted = stored;
    CHECK(avcSessionMatches(stored, wanted, true, true));
    wanted.codedWidth = 1280;
    CHECK(!avcSessionMatches(stored, wanted, true, true));      // geometry change
    wanted = stored;
    wanted.bankBytes = 0;
    CHECK(!avcSessionMatches(stored, wanted, true, true));      // DPB bank layout change
    wanted = stored;
    wanted.maxRefs = 8;
    CHECK(!avcSessionMatches(stored, wanted, true, true));      // reference count change
    wanted = stored;
    wanted.profileIdc = 66;
    CHECK(!avcSessionMatches(stored, wanted, true, true));      // profile change
    CHECK(!avcSessionMatches(stored, wanted, false, true));     // a failed frame forces a recreate
    CHECK(!avcSessionMatches(stored, stored, true, false));     // sysctl off
}

// The fence wait runs on injected primitives, so its scheduling can be checked
// without hardware: the simulated clock only advances inside the wait itself.
static void testVcnWaitStateMachine() {
    using namespace phx_vcn;
    auto run = [](const VcnWaitParams &params, uint64_t doneAtNs, bool updateEwma, VcnWaitResult &out) {
        uint64_t now = 0;
        out = vcnWaitFence(params, [&]() { return now; }, [&]() { return now >= doneAtNs; },
                           [&]() { now += 2000ull; },                       // pause
                           [&](uint64_t ns) { now += ns; },                 // IOLockSleepDeadline
                           [&]() { now += 1000000ull; },                    // IOSleep(1)
                           updateEwma);
    };
    VcnWaitParams params{};
    params.mode = 2;
    params.timeoutMs = 1000;
    params.sliceUs = 50;
    params.spinMaxNs = 200000;
    params.hwEwmaNs = 200000;

    VcnWaitResult result{};
    run(params, 150000, true, result);          // fence retires inside the spin
    CHECK_EQ(result.hit, 1);
    CHECK_EQ(result.sleeps, 0);
    CHECK_EQ(result.elapsedNs, 150000);
    CHECK_EQ(result.hwEwmaNs, 200000 - 25000 + 18750);   // alpha = 1/8 towards 150 us

    params.hwEwmaNs = 1000000;
    run(params, 900000, true, result);          // pre-sleep to 800 us, then spin the rest
    CHECK_EQ(result.hit, 1);
    CHECK_EQ(result.sleeps, 1);
    CHECK_EQ(result.elapsedNs, 900000);
    CHECK_EQ(result.hwEwmaNs, 1000000 - 125000 + 112500);

    params.mode = 1;
    run(params, 3000000, true, result);         // short sleeps only, slice doubles to 1 ms
    CHECK_EQ(result.hit, 2);
    CHECK_EQ(result.sleeps, 7);                 // 1 ms pre-sleep + 50/100/200/400/800/1000 us
    CHECK_EQ(result.elapsedNs, 3550000);        // the last slice overshoots 3 ms

    params.mode = 2;
    params.timeoutMs = 3;
    params.sliceUs = 1000;
    params.hwEwmaNs = 1000000;
    run(params, 1000000000ull, true, result);   // fence never moves: fall back, do not spin forever
    CHECK_EQ(result.hit, 0);
    CHECK_EQ(result.sleeps, 23);                // 20 ms of slices, then 3 x IOSleep(1)
    CHECK_EQ(result.hwEwmaNs, 1000000);         // a timed-out wait must not teach the average

    params.mode = 2;
    params.timeoutMs = 1000;
    params.spinMaxNs = 0;                       // spin disabled by sysctl
    params.hwEwmaNs = 200000;
    run(params, 150000, true, result);
    CHECK_EQ(result.hit, 2);                    // caught after the pre-sleep, not in a spin
    CHECK_EQ(result.sleeps, 1);

    // A session create/close can retire before the first probe lands. That is a
    // hit, not a stuck ring: the old code would return RPTR_STUCK and power the
    // block down in the middle of a stream.
    params.spinMaxNs = 200000;
    params.hwEwmaNs = 200000;
    run(params, 0, true, result);
    CHECK_EQ(result.hit, 1);
    CHECK_EQ(result.sleeps, 0);
    CHECK_EQ(result.elapsedNs, 0);

    // A frame that sat in the fallback for seconds must not poison the average:
    // the input and the sample are both clamped to 4 ms.
    params.hwEwmaNs = 20000000;
    run(params, 15000000, true, result);
    CHECK_EQ(result.hit, 2);
    CHECK_EQ(result.hwEwmaNs, 4000000);
    CHECK(result.spinNs <= 250000);
    CHECK(result.elapsedNs >= 15000000);

    // A fence that is already in when the pre-sleep ends halves the average
    // instead of confirming it. Without this the sleep feeds itself and the
    // average locks at the cap (seen on hardware: one 9 ms spike pinned every
    // later frame at 8 ms of pre-sleep, 223 -> 86 fps).
    params.hwEwmaNs = 8000000;
    run(params, 2000000, true, result);
    CHECK_EQ(result.hit, 2);
    CHECK(result.preSleepHit);
    CHECK_EQ(result.hwEwmaNs, 2000000);   // capped at 4 ms, then halved
    params.hwEwmaNs = 1000000;
    run(params, 100000, true, result);    // long in: half again
    CHECK(result.preSleepHit);
    CHECK_EQ(result.hwEwmaNs, 500000);
    params.hwEwmaNs = 400000;             // still long enough for a pre-sleep
    run(params, 50000, true, result);     // floor is 200 us
    CHECK(result.preSleepHit);
    CHECK_EQ(result.hwEwmaNs, 200000);
}

int main(int argc, char **argv) {
    testAvcSessionReuseKey();
    testVcnWaitStateMachine();
    testApertureTransfers();
    testLayout();
    testDpgList();
    testSessionRingPacket();
    testDecodeSession();
    testDecodeFrame();
    testAvcStream();
    testAvcStreamReferenceSlots();
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
