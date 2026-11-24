// SPDX-License-Identifier: MIT
// Fixed VCN 4 unified-queue decoder messages, matching amdgpu_vcn_dec_sw_ring_test_ib
// in drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c. The create and destroy use the same handle.
#ifndef PHX_VCN_DECODE_H
#define PHX_VCN_DECODE_H

#include <stdint.h>

namespace phx_vcn {

constexpr uint32_t kDecodeCreateDw = 14;
constexpr uint32_t kDecodeDestroyDw = 6;
constexpr uint32_t kDecodeIbDw = 43; // checksum (4), engine (4), buffer package (2 + 33)

constexpr uint32_t kDecodeSessionBytes = 128u * 1024u;
constexpr uint32_t kDecodeDpbBytes = 15923584u;
constexpr uint32_t kDecodeTargetBytes = 737280u;
constexpr uint32_t kDecodeOutputByteSum = 0x020345d8u;
constexpr uint32_t kDecodeFrameWidth = 864u;
constexpr uint32_t kDecodeFrameHeight = 480u;
constexpr uint32_t kDecodeFrameHandle = 0x40440003u;
constexpr uint32_t kDecodeReferenceFrameMessageDw = 59u;
constexpr uint32_t kDecodeReferenceFrameCreateDw = 14u;
constexpr uint32_t kDecodeReferenceFrameDestroyDw = 6u;

// Private BAR0 range above MES/GART, the boot-time PSP TMR and VCN diagnostics.
constexpr uint32_t kDecodeFrameScratchOffset = 0x0d000000u;
constexpr uint32_t kDecodeFrameSessionContextOffset = 0;
constexpr uint32_t kDecodeFrameMessageOffset = kDecodeSessionBytes;
constexpr uint32_t kDecodeFrameFeedbackOffset = kDecodeFrameMessageOffset + 4096u;
constexpr uint32_t kDecodeFrameScalingTableOffset = kDecodeFrameFeedbackOffset + 4096u;
constexpr uint32_t kDecodeFrameBitstreamOffset = kDecodeFrameScalingTableOffset + 4096u;
constexpr uint32_t kDecodeFrameDpbOffset =
    (kDecodeFrameBitstreamOffset + 1408u + 4095u) & ~4095u;
constexpr uint32_t kDecodeFrameContextOffset =
    (kDecodeFrameDpbOffset + 0x006B9400u + 4095u) & ~4095u;
constexpr uint32_t kDecodeFrameTargetOffset =
    (kDecodeFrameDpbOffset + kDecodeDpbBytes + 4095u) & ~4095u;
constexpr uint32_t kDecodeFrameRequiredBytes = kDecodeFrameTargetOffset + kDecodeTargetBytes;
static_assert(kDecodeFrameContextOffset < kDecodeFrameTargetOffset, "decoder context must lie inside DPB");
static_assert(kDecodeFrameScratchOffset + kDecodeFrameRequiredBytes <= 0x10000000u,
              "decoder scratch exceeds Phoenix BAR0");

// Caller clears and owns a page-aligned message page. Return the initialized part of the page.
inline uint32_t buildDecodeSessionMsg(uint32_t *words, bool close, uint32_t width = 1920u,
                                      uint32_t height = 1088u, uint32_t handle = 1u) {
    words[0] = 40; // firmware message header size for both operations
    words[1] = (close ? kDecodeDestroyDw : kDecodeCreateDw) * sizeof(uint32_t);
    words[2] = close ? 0 : 1; // number of indexed submessages
    words[3] = close ? 2 : 0; // destroy / create
    words[4] = handle;
    words[5] = 0; // feedback number
    if (close)
        return kDecodeDestroyDw;

    words[6] = 1; // RDECODE_MESSAGE_CREATE
    words[7] = 40; // submessage offset in bytes
    words[8] = 16; // submessage size in bytes
    words[9] = 0;
    words[10] = 7; // H.264 performance stream
    words[11] = 0; // session flags
    words[12] = width;
    words[13] = height;
    return kDecodeCreateDw;
}

inline uint32_t buildDecodeFrameSessionMsg(uint32_t *words, bool close) {
    if (close) {
        const uint32_t message[] = {40, 24, 0, 2, kDecodeFrameHandle, 0};
        for (uint32_t i = 0; i < kDecodeReferenceFrameDestroyDw; ++i)
            words[i] = message[i];
        return kDecodeReferenceFrameDestroyDw;
    }
    const uint32_t message[] = {
        40, 56, 1, 0, kDecodeFrameHandle, 0, 1, 40, 16, 0, 7, 0,
        kDecodeFrameWidth, kDecodeFrameHeight
    };
    for (uint32_t i = 0; i < kDecodeReferenceFrameCreateDw; ++i)
        words[i] = message[i];
    return kDecodeReferenceFrameCreateDw;
}

struct DecodeFrameBuffers {
    uint64_t message;
    uint64_t dpb;
    uint64_t target;
    uint64_t sessionContext;
    uint64_t bitstream;
    uint64_t context;
    uint64_t feedback;
    uint64_t scalingTable;
};

inline DecodeFrameBuffers makeDecodeFrameBuffers(uint64_t scratchMc) {
    return {
        scratchMc + kDecodeFrameMessageOffset,
        scratchMc + kDecodeFrameDpbOffset,
        scratchMc + kDecodeFrameTargetOffset,
        scratchMc + kDecodeFrameSessionContextOffset,
        scratchMc + kDecodeFrameBitstreamOffset,
        scratchMc + kDecodeFrameContextOffset,
        scratchMc + kDecodeFrameFeedbackOffset,
        scratchMc + kDecodeFrameScalingTableOffset
    };
}

// Firmware reads the buffer addresses as high dword followed by low dword.
inline void writeDecodeAddress(uint32_t *words, uint64_t mc) {
    words[0] = static_cast<uint32_t>(mc >> 32);
    words[1] = static_cast<uint32_t>(mc);
}

// libdrm tests/amdgpu/vcn_tests.c, rvcn_decode_buffer_s and vcn_dec_cmd.
// Optional session context is used by both session management IBs of the libdrm frame test.
inline uint32_t buildDecodeIb(uint32_t *words, uint64_t msgMc, uint64_t sessionMc,
                              const DecodeFrameBuffers *frame) {
    const uint32_t payloadDw = kDecodeIbDw - 4u;
    for (uint32_t i = 0; i < kDecodeIbDw; ++i)
        words[i] = 0;
    words[0] = 16;
    words[1] = 0x30000002; // single queue checksum
    words[3] = payloadDw;
    words[4] = 16;
    words[5] = 0x30000001; // engine info
    words[6] = 3; // decode
    words[7] = payloadDw * sizeof(uint32_t);
    words[8] = (kDecodeIbDw - 8u) * sizeof(uint32_t);
    words[9] = 1;  // DECODE_IB_PARAM_DECODE_BUFFER
    words[10] = 1; // MSG_BUFFER
    writeDecodeAddress(words + 11, msgMc);
    if (sessionMc) {
        words[10] |= 0x00100000; // SESSION_CONTEXT_BUFFER
        writeDecodeAddress(words + 17, sessionMc);
    }
    if (frame) {
        words[10] |= 0x00000a1e; // DPB, bitstream, target, feedback, IT scaling and decoder context
        writeDecodeAddress(words + 13, frame->dpb);
        writeDecodeAddress(words + 15, frame->target);
        writeDecodeAddress(words + 19, frame->bitstream);
        writeDecodeAddress(words + 21, frame->context);
        writeDecodeAddress(words + 23, frame->feedback);
        writeDecodeAddress(words + 31, frame->scalingTable);
    }
    for (uint32_t i = 4; i < kDecodeIbDw; ++i)
        words[2] += words[i];
    return kDecodeIbDw;
}

inline uint32_t buildDecodeSessionIb(uint32_t *words, uint64_t msgMc, uint64_t sessionMc = 0) {
    return buildDecodeIb(words, msgMc, sessionMc, nullptr);
}

inline uint32_t buildDecodeFrameIb(uint32_t *words, const DecodeFrameBuffers &buffers) {
    return buildDecodeIb(words, buffers.message, buffers.sessionContext, &buffers);
}

} // namespace phx_vcn

#endif // PHX_VCN_DECODE_H
