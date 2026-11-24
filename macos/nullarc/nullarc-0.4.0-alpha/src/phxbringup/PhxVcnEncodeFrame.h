// SPDX-License-Identifier: MIT
// Copyright 2017 Advanced Micro Devices, Inc. (libdrm reference packets; MIT).
// H.264 IDR encode submission for the VCN 4 unified queue.
// Packet layout follows AMD's libdrm tests/amdgpu/vcn_tests.c, encode-frame path.
#ifndef PHX_VCN_ENCODE_FRAME_H
#define PHX_VCN_ENCODE_FRAME_H

#include "PhxVcn.h"
#include "PhxVcnUserABI.h"

namespace phx_vcn {

constexpr uint32_t kEncodeFrameWidth = PHXN_VCN_WIDTH;
constexpr uint32_t kEncodeFrameHeight = PHXN_VCN_HEIGHT;
constexpr uint32_t kEncodeFramePitch = 256u;
constexpr uint32_t kEncodeFrameLumaBytes = kEncodeFramePitch * kEncodeFrameHeight;
constexpr uint32_t kEncodeFrameInputBytes = kEncodeFrameLumaBytes * 3u / 2u;
constexpr uint32_t kEncodeFrameCpbBytes = 2u * kEncodeFrameInputBytes;
constexpr uint32_t kEncodeFrameBitstreamBytes = PHXN_VCN_H264_CAPACITY;
constexpr uint32_t kEncodeFrameFeedbackBytes = 4096u;

// Addresses passed to the firmware are offsets relative to the same BAR0 VRAM aperture as PhxVcn.h.
// The output limit is programmed in both firmware packages. The last BO page holds feedback.
constexpr uint32_t kEncodeFrameCpbOff = kCodecDataOff + kEncodeContextBytes;
constexpr uint32_t kEncodeFrameInputOff = kEncodeFrameCpbOff + kEncodeFrameCpbBytes;
constexpr uint32_t kEncodeFrameBitstreamOff = kEncodeFrameInputOff + kEncodeFrameInputBytes;
constexpr uint32_t kEncodeFrameFeedbackOff = kEncodeFrameBitstreamOff + kEncodeFrameBitstreamBytes;
constexpr uint32_t kEncodeFrameRequiredBoSize = kEncodeFrameFeedbackOff + kEncodeFrameFeedbackBytes;

// The legacy path keeps the exact libdrm layout. Larger frames share the
// decoder's scratch aperture, exclusively owned under vcnLock through close.
constexpr uint32_t kEncodeScratchMax = 26u << 20;
struct EncodeGeometry {
    uint32_t width = kEncodeFrameWidth, height = kEncodeFrameHeight;
    uint32_t codedWidth = kEncodeFrameWidth, codedHeight = kEncodeFrameHeight;
    uint32_t pitch = kEncodeFramePitch, lumaBytes = kEncodeFrameLumaBytes;
    uint32_t recLumaBytes = kEncodeFrameLumaBytes, bitstreamBytes = kEncodeFrameBitstreamBytes;
    uint32_t cpbOff = kEncodeFrameCpbOff, inputOff = kEncodeFrameInputOff;
    uint32_t bitstreamOff = kEncodeFrameBitstreamOff, feedbackOff = kEncodeFrameFeedbackOff;
    uint32_t scratchBytes = 0, level = 11;
};

inline bool sizedEncodeGeometry(uint32_t width, uint32_t height, EncodeGeometry &g) {
    if (!phxnVcnSizeSupported(width, height)) return false;
    g.width = width;
    g.height = height;
    g.codedWidth = (width + 15u) & ~15u;
    g.codedHeight = (height + 15u) & ~15u;
    g.pitch = (width + 255u) & ~255u;
    g.lumaBytes = g.pitch * g.codedHeight;
    g.recLumaBytes = g.pitch * (g.codedHeight < 256u ? 256u : g.codedHeight);
    g.bitstreamBytes = phxnVcnBitstreamCapacity(width, height);
    g.cpbOff = 0;
    g.inputOff = 3u * g.recLumaBytes; // two reconstructed NV12 pictures
    g.bitstreamOff = g.inputOff + g.lumaBytes * 3u / 2u;
    g.feedbackOff = (g.bitstreamOff + g.bitstreamBytes + 4095u) & ~4095u;
    g.scratchBytes = g.feedbackOff + kEncodeFrameFeedbackBytes;
    // Allow up to 60 frames/second for the selected level's macroblock rate.
    uint32_t mb = (g.codedWidth / 16u) * (g.codedHeight / 16u);
    g.level = mb <= 1800u ? 31u : mb <= 3600u ? 32u : mb <= 8192u ? 42u : 52u;
    return g.scratchBytes <= kEncodeScratchMax;
}

// Count includes the eight-dword unified header and every firmware package.
constexpr uint32_t kEncodeFrameIbDw = 449u;
constexpr uint32_t kEncodeFrameMaxIbDw = kEncodeFrameIbDw + 12u;
constexpr uint32_t kEncodeFrameOpenIbDw = 99u;
constexpr uint32_t kEncodeFrameCloseIbDw = 21u;
static_assert(kEncodeFrameMaxIbDw * sizeof(uint32_t) <= kCodecDataOff - kCodecIbOff, "VCN frame IB space");
static_assert(kEncodeFrameRequiredBoSize <= kBoSize, "VCN frame exceeds the reserved BO");

inline void encodeFramePackageHeader(uint32_t *words, uint32_t &cursor, uint32_t tag,
                                     uint32_t &packageStart) {
    packageStart = cursor;
    words[cursor++] = 0;
    words[cursor++] = tag;
}

inline void encodeFramePackageFinish(uint32_t *words, uint32_t packageStart, uint32_t cursor) {
    words[packageStart] = (cursor - packageStart) * sizeof(uint32_t);
}

// Fills a pitch-padded NV12 image with four large luma quadrants and neutral chroma.
// The simple pattern is deterministic and exposes orientation, plane, and pitch mistakes.
inline bool initializeEncodeFrame(volatile uint8_t *pixels, uint32_t capacity) {
    if (!pixels || capacity < kEncodeFrameInputBytes)
        return false;
    for (uint32_t y = 0; y < kEncodeFrameHeight; ++y) {
        volatile uint8_t *row = pixels + y * kEncodeFramePitch;
        const bool bottom = y >= kEncodeFrameHeight / 2u;
        for (uint32_t x = 0; x < kEncodeFrameWidth; ++x) {
            const bool bright = ((x >= kEncodeFrameWidth / 2u) != bottom);
            row[x] = bright ? 200u : 48u;
        }
        for (uint32_t x = kEncodeFrameWidth; x < kEncodeFramePitch; ++x)
            row[x] = 16u;
    }
    for (uint32_t y = 0; y < kEncodeFrameHeight / 2u; ++y) {
        volatile uint8_t *row = pixels + kEncodeFrameLumaBytes + y * kEncodeFramePitch;
        for (uint32_t x = 0; x < kEncodeFramePitch; ++x)
            row[x] = 128u;
    }
    return true;
}

// Input pitches are in plane elements (Y samples or interleaved UV pairs).
inline bool copyPackedNv12ToEncodeFrame(volatile uint8_t *pixels, uint32_t capacity,
                                        const uint8_t *packed, uint32_t length, const EncodeGeometry &g = {}) {
    if (!pixels || capacity < g.lumaBytes * 3u / 2u || !packed || length != g.width * g.height * 3u / 2u)
        return false;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const uint32_t rows = g.height >> plane, codedRows = g.codedHeight >> plane;
        const uint8_t *src = packed + (plane ? g.width * g.height : 0u);
        volatile uint8_t *dst = pixels + (plane ? g.lumaBytes : 0u);
        for (uint32_t y = 0; y < codedRows; ++y) {
            uint32_t x = 0;
            if (y < rows) {
                // BAR0 is a prefetchable framebuffer aperture. Copy each visible
                // row in bulk so 1440p input does not take one MMIO write per byte.
                __builtin_memcpy(const_cast<uint8_t *>(dst), src, g.width);
                x = g.width;
                src += g.width;
            }
            __builtin_memset(const_cast<uint8_t *>(dst) + x, plane ? 128u : 16u, g.pitch - x);
            dst += g.pitch;
        }
    }
    return true;
}

// Common ABI 1.9 session and task headers for the real initialize, encode, and close operations.
inline uint32_t encodeFrameIbHeader(uint32_t *words, uint64_t contextMc, uint32_t taskId,
                                    bool feedback) {
    words[0] = 16;
    words[1] = 0x30000002; // unified checksum
    words[4] = 16;
    words[5] = 0x30000001; // engine info
    words[6] = 2; // unified queue encode engine
    words[8] = 24;
    words[9] = 1; // session info
    words[10] = 0x00010009; // encoder firmware ABI 1.9
    words[11] = static_cast<uint32_t>(contextMc >> 32);
    words[12] = static_cast<uint32_t>(contextMc);
    words[13] = 1; // RENCODE_ENGINE_TYPE_ENCODE
    words[14] = 20;
    words[15] = 2; // task info
    words[17] = taskId;
    words[18] = feedback ? 1u : 0u;
    return 19u;
}

inline uint32_t finishEncodeFrameIb(uint32_t *words, uint32_t count) {
    words[2] = 0;
    words[3] = count - 4u;
    words[7] = words[3] * sizeof(uint32_t);
    words[16] = (count - 14u) * sizeof(uint32_t); // task bytes starting at task info
    for (uint32_t i = 4u; i < count; ++i)
        words[2] += words[i];
    return count;
}

inline uint32_t buildEncodeFrameSessionIb(uint32_t *words, uint64_t contextMc,
                                          uint32_t taskId, bool close, const EncodeGeometry &g = {}) {
    uint32_t cursor = encodeFrameIbHeader(words, contextMc, taskId, false);
    if (close) {
        words[cursor++] = 8;
        words[cursor++] = 0x01000002; // close session
    } else {
        // Length in bytes, tag, then payload for each firmware package.
        const uint32_t init[] = {
            8, 0x01000001, // initialize
            44, 3, 1, g.codedWidth, g.codedHeight, g.codedWidth - g.width, g.codedHeight - g.height, 0, 0, 0, 0,
            16, 0x00200001, 0, (g.codedWidth / 16) * (g.codedHeight / 16), // single slice
            44, 0x00200002, 0, 0, 0, 1, 1, 100, g.level, 0, 0, // H.264 High profile
            28, 0x00200004, 0, 0, 0, 0, 0, // deblocking filter
            16, 4, 1, 1, // layer control
            16, 6, 0, 48, // rate-control session
            28, 9, 0, 0, 0, 0, 0, // quality parameters
            12, 5, 0, // select temporal layer 0
            40, 7, 0, 0, 25, 1, 0x01312d00, 0, 0, 0, // rate-control layer
            12, 5, 0, // select temporal layer 0
            40, 8, 20, 0, 51, 0, 1, 0, 1, 0, // per-picture rate control
            8, 0x01000004, // initialize rate control
            8, 0x01000005, // initialize VBV level
        };
        static_assert(19u + sizeof(init) / sizeof(init[0]) == kEncodeFrameOpenIbDw,
                      "VCN encoder initialization package size");
        for (uint32_t value : init)
            words[cursor++] = value;
    }
    return finishEncodeFrameIb(words, cursor);
}

// RBSP for the same High-profile PPS and IDR slice template used by libdrm.
// The largest supported dimensions need fewer than 24 bytes before escaping.
struct EncodeSpsBits {
    uint8_t bytes[32]{};
    uint32_t count = 0;
    void bit(uint32_t value) {
        bytes[count / 8u] |= (value & 1u) << (7u - count % 8u);
        ++count;
    }
    void ue(uint32_t value) {
        uint32_t bits = 0;
        for (uint32_t n = value + 1u; n; n >>= 1u) ++bits;
        for (uint32_t i = 1u; i < bits; ++i) bit(0u);
        while (bits) bit((value + 1u) >> --bits);
    }
};

inline uint32_t buildSizedSps(const EncodeGeometry &g, uint8_t nal[48]) {
    EncodeSpsBits s;
    s.ue(0); s.ue(1); // SPS id, chroma_format_idc (4:2:0)
    s.ue(0); s.ue(0); s.bit(0); s.bit(0); // 8-bit, no QP bypass/scaling matrices
    s.ue(1); s.ue(0); s.ue(1); // five-bit frame_num and type-0 five-bit POC
    s.ue(2); s.bit(0); // reference frames, gaps flag
    s.ue(g.codedWidth / 16u - 1u);
    s.ue(g.codedHeight / 16u - 1u);
    s.bit(1); s.bit(1); // progressive, direct_8x8_inference
    bool crop = g.codedWidth != g.width || g.codedHeight != g.height;
    s.bit(crop);
    if (crop) {
        s.ue(0); s.ue((g.codedWidth - g.width) / 2u);
        s.ue(0); s.ue((g.codedHeight - g.height) / 2u);
    }
    s.bit(0); // VUI absent (NV12 video-range default)
    s.bit(1); // rbsp_stop_one_bit, followed by zero padding
    const uint8_t prefix[] = {0, 0, 0, 1, 0x67, 100, 0x44, static_cast<uint8_t>(g.level)};
    uint32_t length = 0, zeros = 0;
    for (uint8_t byte : prefix) nal[length++] = byte;
    for (uint32_t i = 0; i < (s.count + 7u) / 8u; ++i) {
        const uint8_t byte = s.bytes[i];
        if (zeros == 2u && byte <= 3u) { nal[length++] = 3u; zeros = 0; }
        nal[length++] = byte;
        zeros = byte == 0u ? zeros + 1u : 0u;
    }
    return length;
}

// Caller supplies kEncodeFrameMaxIbDw dwords for sized frames and holds the MC ranges through the fence.
inline uint32_t buildEncodeFrameIb(uint32_t *words, uint64_t contextMc, uint64_t cpbMc,
                                   uint64_t inputMc, uint64_t bitstreamMc, uint64_t feedbackMc,
                                   uint32_t taskId, const EncodeGeometry &g = {}) {
    uint32_t cursor = encodeFrameIbHeader(words, contextMc, taskId, true);
    uint32_t packageStart = 0;
    encodeFramePackageHeader(words, cursor, 0x0au, packageStart); // direct SPS NAL output
    words[cursor++] = 2u;
    if (g.scratchBytes) {
        uint8_t nal[48]{};
        uint32_t length = buildSizedSps(g, nal);
        words[cursor++] = length;
        for (uint32_t i = 0; i < length; i += 4u)
            words[cursor++] = (uint32_t(nal[i]) << 24) | (uint32_t(nal[i + 1u]) << 16) |
                              (uint32_t(nal[i + 2u]) << 8) | nal[i + 3u];
    } else {
        words[cursor++] = 17u;
        words[cursor++] = 1u;
        words[cursor++] = 0x6764440bu;
        words[cursor++] = 0xac54c284u;
        words[cursor++] = 0x68078442u;
        words[cursor++] = 0x37000000u;
    }
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x0au, packageStart); // direct PPS NAL output
    words[cursor++] = 3u;
    words[cursor++] = 8u;
    words[cursor++] = 1u;
    words[cursor++] = 0x68ce3c80u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x0bu, packageStart); // H.264 IDR slice header
    words[cursor++] = 0x65000000u;
    words[cursor++] = 0x11040000u;
    words[cursor++] = 0xe0000000u;
    for (uint32_t i = 0; i < 13u; ++i)
        words[cursor++] = 0u;
    const uint32_t sliceSyntax[] = {1u, 8u, 0x00020000u, 0u, 1u, 0x15u,
                                    0x00020001u, 0u, 1u, 3u};
    for (uint32_t value : sliceSyntax)
        words[cursor++] = value;
    for (uint32_t i = 0; i < 22u; ++i)
        words[cursor++] = 0u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x0fu, packageStart); // encode parameters
    words[cursor++] = 2u; // IDR frame
    words[cursor++] = g.bitstreamBytes;
    words[cursor++] = static_cast<uint32_t>(inputMc >> 32);
    words[cursor++] = static_cast<uint32_t>(inputMc);
    const uint64_t chromaMc = inputMc + g.lumaBytes;
    words[cursor++] = static_cast<uint32_t>(chromaMc >> 32);
    words[cursor++] = static_cast<uint32_t>(chromaMc);
    words[cursor++] = g.pitch;
    words[cursor++] = g.pitch / 2u; // chroma pitch in RG8 elements
    words[cursor++] = 0u;
    words[cursor++] = 0xffffffffu;
    words[cursor++] = 0u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x00200003u, packageStart); // H.264 encode parameters
    for (uint32_t i = 0; i < 7u; ++i)
        words[cursor++] = 0u;
    words[cursor++] = 0xffffffffu;
    for (uint32_t i = 0; i < 4u; ++i)
        words[cursor++] = 0u;
    words[cursor++] = 0xffffffffu;
    for (uint32_t i = 0; i < 4u; ++i)
        words[cursor++] = 0u;
    words[cursor++] = 1u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x11u, packageStart); // encode context
    words[cursor++] = static_cast<uint32_t>(cpbMc >> 32);
    words[cursor++] = static_cast<uint32_t>(cpbMc);
    words[cursor++] = 0u; // linear swizzle
    words[cursor++] = g.pitch;
    words[cursor++] = g.pitch;
    words[cursor++] = 2u; // no reconstructed picture
    words[cursor++] = 0u;
    words[cursor++] = g.recLumaBytes;
    words[cursor++] = 0u;
    words[cursor++] = 0u;
    words[cursor++] = g.recLumaBytes * 3u / 2u;
    words[cursor++] = g.recLumaBytes * 5u / 2u;
    for (uint32_t i = 0; i < 280u; ++i)
        words[cursor++] = 0u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x12u, packageStart); // output bitstream buffer
    words[cursor++] = 0u;
    words[cursor++] = static_cast<uint32_t>(bitstreamMc >> 32);
    words[cursor++] = static_cast<uint32_t>(bitstreamMc);
    words[cursor++] = g.bitstreamBytes;
    words[cursor++] = 0u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x15u, packageStart); // feedback buffer
    words[cursor++] = 0u;
    words[cursor++] = static_cast<uint32_t>(feedbackMc >> 32);
    words[cursor++] = static_cast<uint32_t>(feedbackMc);
    words[cursor++] = 0x10u;
    words[cursor++] = 0x28u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x10u, packageStart); // intra refresh
    words[cursor++] = 0u;
    words[cursor++] = 0u;
    words[cursor++] = 0u;
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x0cu, packageStart); // NV12 input color format
    for (uint32_t i = 0; i < 5u; ++i)
        words[cursor++] = 0u;
    words[cursor++] = 0u; // 8-bit
    words[cursor++] = 0u; // NV12
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x0du, packageStart); // bitstream color format
    for (uint32_t i = 0; i < 3u; ++i)
        words[cursor++] = 0u;
    words[cursor++] = 0u; // 8-bit
    encodeFramePackageFinish(words, packageStart, cursor);

    encodeFramePackageHeader(words, cursor, 0x01000006u, packageStart); // speed mode
    encodeFramePackageFinish(words, packageStart, cursor);
    encodeFramePackageHeader(words, cursor, 0x01000003u, packageStart); // encode frame
    encodeFramePackageFinish(words, packageStart, cursor);

    if (cursor > kEncodeFrameMaxIbDw)
        return 0;
    return finishEncodeFrameIb(words, cursor);
}

inline uint32_t readLe32(const volatile uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

inline bool findH264StartCode(const volatile uint8_t *bytes, uint32_t size, uint32_t from,
                              uint32_t &start, uint32_t &prefixBytes) {
    if (size < 3u)
        return false;
    for (uint32_t i = from; i <= size - 3u; ++i) {
        if (size - i >= 4u && bytes[i] == 0u && bytes[i + 1u] == 0u &&
            bytes[i + 2u] == 0u && bytes[i + 3u] == 1u) {
            start = i;
            prefixBytes = 4u;
            return true;
        }
        if (bytes[i] == 0u && bytes[i + 1u] == 0u && bytes[i + 2u] == 1u) {
            start = i;
            prefixBytes = 3u;
            return true;
        }
    }
    return false;
}

// Validates the reference feedback length and requires Annex-B SPS, PPS and IDR NAL units.
inline bool encodedH264IdrOk(const volatile uint8_t *feedback, uint32_t feedbackCapacity,
                             const volatile uint8_t *bytes, uint32_t bitstreamCapacity,
                             uint32_t *encodedBytes) {
    if (encodedBytes)
        *encodedBytes = 0u;
    if (!feedback || feedbackCapacity < 7u * sizeof(uint32_t) || !bytes ||
        bitstreamCapacity == 0u)
        return false;

    // This diagnostic writes SPS, PPS and IDR from byte 0; dword 6 gives the end.
    // Phoenix returned dword 8 = 5 for a verified stream starting at byte 0.
    if (!readLe32(feedback + sizeof(uint32_t)))
        return false;
    const uint32_t size = readLe32(feedback + 6u * sizeof(uint32_t));
    if (size == 0u || size > bitstreamCapacity)
        return false;

    uint32_t firstStart = 0, firstPrefix = 0;
    if (!findH264StartCode(bytes, size, 0u, firstStart, firstPrefix))
        return false;
    for (uint32_t i = 0; i < firstStart; ++i) {
        if (bytes[i] != 0u)
            return false;
    }

    bool haveSps = false, havePps = false, haveIdr = false;
    uint32_t start = firstStart, prefix = firstPrefix;
    for (;;) {
        const uint32_t nal = start + prefix;
        uint32_t nextStart = 0, nextPrefix = 0;
        const bool hasNext = findH264StartCode(bytes, size, nal, nextStart, nextPrefix);
        uint32_t nalEnd = hasNext ? nextStart : size;
        while (nalEnd > nal && bytes[nalEnd - 1u] == 0u)
            --nalEnd;
        if (nalEnd - nal < 2u || (bytes[nal] & 0x80u) != 0u)
            return false;
        const uint8_t type = bytes[nal] & 0x1fu;
        if (type == 0u || type > 23u)
            return false;
        if ((type == 8u && !haveSps) || (type == 5u && !havePps))
            return false;
        haveSps = haveSps || type == 7u;
        havePps = havePps || type == 8u;
        haveIdr = haveIdr || type == 5u;
        if (!hasNext)
            break;
        start = nextStart;
        prefix = nextPrefix;
    }

    if (!haveIdr)
        return false;
    if (encodedBytes)
        *encodedBytes = size;
    return true;
}

} // namespace phx_vcn

#endif // PHX_VCN_ENCODE_FRAME_H
