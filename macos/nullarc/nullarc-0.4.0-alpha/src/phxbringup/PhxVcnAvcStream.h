// SPDX-License-Identifier: MIT
#ifndef PHX_VCN_AVC_STREAM_H
#define PHX_VCN_AVC_STREAM_H

#include "PhxVcnDecode.h"

namespace phx_vcn {

constexpr uint32_t kAvcStreamMaxBitstreamBytes = 1024u * 1024u;
constexpr uint32_t kAvcStreamAlignment = 4096u;
constexpr uint32_t kAvcStreamScratchOffset = 234u * 1024u * 1024u;
constexpr uint32_t kAvcStreamScratchLimit = 22u * 1024u * 1024u;
// Session handles. Memory is no longer tied to a handle: each session takes what its stream needs from the two
// pools below (avcPoolAlloc), so many small streams or a few 1080p ones fit in the same VRAM.
constexpr uint32_t kAvcStreamSlotCount = 6;
// Upper bound for one session's persistent area (firmware session + codec context [+ DPB]).
// 1080p with eight references (live streams at level 5.0) needs 13.4 MiB of context.
constexpr uint32_t kAvcStreamSlotBytes = 16u << 20;
// Context pool, CPU-visible inside the BAR0 aperture, 80..128 MiB (below the 200 MiB BO window): the
// same 48 MiB window build 79 used. Build 80 grew it to 80..176 MiB and the next boot lost the Metal
// accelerator, so nothing beyond 128 MiB is claimed.
constexpr uint32_t kAvcContextBankOffset = 80u << 20;
constexpr uint32_t kAvcContextPoolBytes = 48u << 20;
// Reference pictures live in a GPU-only pool outside the aperture. PhxAMDFB places its second
// scan-out surface at 512 MiB (phxfb.cpp gDbufOffMb), so start at 1 GiB; 192 MiB as in build 79.
constexpr uint32_t kAvcDpbBankOffset = 1024u << 20;
constexpr uint32_t kAvcDpbBankBytes = 32u << 20;   // upper bound for one session
constexpr uint32_t kAvcDpbPoolBytes = 192u << 20;
constexpr uint32_t kAvcPoolAlignment = 64u << 10;
static_assert(kAvcStreamSlotBytes % kAvcStreamAlignment == 0, "AVC slot alignment");
static_assert(kAvcContextBankOffset + kAvcContextPoolBytes <= 128u << 20, "AVC context pool must stay below 128 MiB");
static_assert(kAvcContextPoolBytes >= kAvcStreamSlotBytes && kAvcDpbPoolBytes >= kAvcDpbBankBytes, "AVC pool size");

struct AvcRange { uint32_t offset, bytes; };

// First-fit placement of `bytes` inside [poolOffset, poolOffset + poolBytes), avoiding the live ranges
// (bytes == 0 means unused). On success returns true with the aligned start in `offset`.
inline bool avcPoolAlloc(uint32_t poolOffset, uint32_t poolBytes, const AvcRange *used, uint32_t count,
                         uint32_t bytes, uint32_t &offset) {
    if (!bytes || bytes > poolBytes) return false;
    const uint64_t end = static_cast<uint64_t>(poolOffset) + poolBytes;
    const uint32_t size = (bytes + kAvcPoolAlignment - 1u) & ~(kAvcPoolAlignment - 1u);
    uint64_t candidate = poolOffset;
    for (uint32_t pass = 0; pass <= count; ++pass) {
        bool moved = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!used[i].bytes) continue;
            const uint64_t start = used[i].offset, stop = start + used[i].bytes;
            if (candidate < stop && start < candidate + size) {
                candidate = (stop + kAvcPoolAlignment - 1u) & ~static_cast<uint64_t>(kAvcPoolAlignment - 1u);
                moved = true;
            }
        }
        if (!moved) break;
    }
    if (candidate + size > end) return false;
    offset = static_cast<uint32_t>(candidate);
    return true;
}

constexpr uint32_t kAvcStreamMessageDw = 342u;
constexpr uint32_t kAvcStreamFeedbackDw = 11u;
constexpr uint32_t kAvcStreamScalingBytes = 224u;

// Parsed H.264 syntax. The kernel owns the handle, reference state, sizes and all GPU addresses.
struct AvcStreamSequence {
    uint32_t codedWidth, codedHeight, visibleWidth, visibleHeight;
    uint8_t profileIdc, levelIdc, log2MaxFrameNumMinus4, picOrderCntType;
    uint8_t log2MaxPicOrderCntLsbMinus4, maxNumRefFrames;
    bool direct8x8Inference;
};

constexpr uint32_t kAvcMaxReferences = 16; // H.264 limit; the VCN message carries 16 entries.

struct AvcReference {
    uint32_t frameNum;
    int32_t topPoc, bottomPoc;
    uint8_t slot;
};

struct AvcStreamPicture {
    uint32_t frameNum;
    int32_t topPoc, bottomPoc;
    AvcReference references[kAvcMaxReferences];
    uint8_t referenceCount, currentSlot;
    uint8_t l0DefaultMinus1, l1DefaultMinus1, weightedBipredIdc;
    int8_t picInitQpMinus26, chromaQpIndexOffset, secondChromaQpIndexOffset;
    bool idr, entropyCoding, bottomFieldPocPresent, deblockingControlPresent;
    bool constrainedIntraPred, weightedPred, transform8x8;
};

struct AvcStreamGeometry {
    uint32_t pitch, lumaBytes, targetBytes;
    uint32_t dpbPitch, dpbChromaPitch, dpbHeight, dpbBytes, contextBytes;
    uint32_t messageOffset, feedbackOffset, scalingOffset, bitstreamOffset;
    uint32_t targetOffset, stagingBytes;
    uint32_t dpbOffset, contextOffset, persistentBytes;
    bool separateDpb;
};

constexpr uint32_t avcStreamAlign(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

inline bool avcStreamGeometry(const AvcStreamSequence &sequence, AvcStreamGeometry &geometry) {
    geometry = {};
    if (sequence.codedWidth < 64 || sequence.codedWidth > 1920 || sequence.codedWidth % 16 ||
        sequence.codedHeight < 64 || sequence.codedHeight > 1088 || sequence.codedHeight % 16 ||
        sequence.visibleWidth < 64 || sequence.visibleWidth > sequence.codedWidth || sequence.visibleWidth % 2 ||
        sequence.visibleHeight < 64 || sequence.visibleHeight > sequence.codedHeight || sequence.visibleHeight % 2 ||
        (sequence.profileIdc != 66 && sequence.profileIdc != 77 && sequence.profileIdc != 100) ||
        sequence.log2MaxFrameNumMinus4 > 12 || sequence.maxNumRefFrames > kAvcMaxReferences ||
        (sequence.picOrderCntType != 0 && sequence.picOrderCntType != 2) ||
        sequence.log2MaxPicOrderCntLsbMinus4 > 12 ||
        (sequence.picOrderCntType == 2 && sequence.log2MaxPicOrderCntLsbMinus4)) return false;

    // H.264 Annex A maximum frame sizes in macroblocks.
    struct Level { uint8_t idc; uint32_t maxFrameMbs, maxDpbMbs; };
    constexpr Level levels[] = {{10, 99, 396}, {11, 396, 900}, {12, 396, 2376}, {13, 396, 2376},
        {20, 396, 2376}, {21, 792, 4752}, {22, 1620, 8100}, {30, 1620, 8100}, {31, 3600, 18000},
        {32, 5120, 20480}, {40, 8192, 32768}, {41, 8192, 32768}, {42, 8704, 34816},
        // Live "high frame rate" 1080p60 streams signal level 5.0 (sometimes 5.1) with eight
        // reference frames; they are valid pictures and fit the DPB bank below.
        {50, 22080, 110400}, {51, 36864, 184320}, {52, 36864, 184320}};
    const uint32_t widthMbs = sequence.codedWidth / 16u;
    const uint32_t heightMbs = sequence.codedHeight / 16u;
    uint32_t maxFrameMbs = 0, maxDpbMbs = 0;
    for (const Level &level : levels) {
        if (level.idc == sequence.levelIdc) {
            maxFrameMbs = level.maxFrameMbs;
            maxDpbMbs = level.maxDpbMbs;
            break;
        }
    }
    if (widthMbs * heightMbs > maxFrameMbs || widthMbs * widthMbs > 8u * maxFrameMbs ||
        heightMbs * heightMbs > 8u * maxFrameMbs ||
        widthMbs * heightMbs * sequence.maxNumRefFrames > maxDpbMbs) return false;

    // Tier 0 must keep the pre-decode DPB intact until the current picture is complete.
    const uint32_t slots = sequence.maxNumRefFrames + 1u;

    AvcStreamGeometry candidate{};
    candidate.pitch = avcStreamAlign(sequence.codedWidth, 256u);
    candidate.lumaBytes = candidate.pitch * sequence.codedHeight;
    candidate.targetBytes = candidate.lumaBytes * 3u / 2u;
    candidate.dpbPitch = avcStreamAlign(sequence.codedWidth, 32u);
    candidate.dpbChromaPitch = avcStreamAlign(sequence.codedWidth / 2u, 32u);
    candidate.dpbHeight = avcStreamAlign(sequence.codedHeight, 64u);
    const uint32_t dpbLumaBytes = candidate.dpbPitch * candidate.dpbHeight;
    // UV pitch counts two-byte pairs. The chroma plane has half the luma rows.
    const uint32_t dpbChromaBytes = candidate.dpbChromaPitch * 2u * (candidate.dpbHeight / 2u);
    candidate.dpbBytes = slots * avcStreamAlign(dpbLumaBytes + dpbChromaBytes, 1024u);
    const uint32_t contextMbs = widthMbs * avcStreamAlign(heightMbs, 2u);
    candidate.contextBytes = slots * avcStreamAlign(contextMbs * 192u, 256u);
    // vcnLock covers the complete submit, fence, and readback. These buffers
    // can reuse the legacy frame scratch on every invocation.
    candidate.messageOffset = 0;
    candidate.feedbackOffset = candidate.messageOffset + kAvcStreamAlignment;
    candidate.scalingOffset = candidate.feedbackOffset + kAvcStreamAlignment;
    candidate.bitstreamOffset = candidate.scalingOffset + kAvcStreamAlignment;
    candidate.targetOffset = candidate.bitstreamOffset + kAvcStreamMaxBitstreamBytes;
    candidate.stagingBytes = avcStreamAlign(candidate.targetOffset + candidate.targetBytes, kAvcStreamAlignment);
    // The firmware session, its reference pictures and its codec context must
    // survive intervening frames from other streams.
    // Reference pictures go to the GPU-only DPB pool whenever they fit one bank, so the scarce CPU-visible
    // context pool only holds the firmware session and the codec context. Inline DPB (the old small-stream
    // layout) is kept as a fallback.
    candidate.dpbOffset = kDecodeSessionBytes;
    candidate.contextOffset = avcStreamAlign(candidate.dpbOffset + candidate.dpbBytes, kAvcStreamAlignment);
    candidate.persistentBytes = avcStreamAlign(candidate.contextOffset + candidate.contextBytes, kAvcStreamAlignment);
    if (candidate.dpbBytes <= kAvcDpbBankBytes) {
        candidate.separateDpb = true;
        candidate.dpbOffset = 0;
        candidate.contextOffset = kDecodeSessionBytes;
        candidate.persistentBytes = avcStreamAlign(candidate.contextOffset + candidate.contextBytes, kAvcStreamAlignment);
    }
    if (candidate.persistentBytes > kAvcStreamSlotBytes ||
        candidate.stagingBytes > kAvcStreamScratchOffset - kDecodeFrameScratchOffset) return false;
    geometry = candidate;
    return true;
}

// The firmware session's identity. An IDR may reuse the session in place when
// everything the create message fixed is unchanged: the geometry the buffers
// were sized for, the profile, and the reference picture count. The parser
// already empties the reference list on an IDR, so the per-frame message fully
// describes the fresh state (Mesa never recreates its session on an IDR either).
struct AvcSessionKey {
    uint32_t codedWidth, codedHeight;
    uint32_t persistentBytes, stagingBytes, bankBytes;
    uint8_t profileIdc, maxRefs;
};

inline bool avcSessionMatches(const AvcSessionKey &stored, const AvcSessionKey &wanted, bool errorFree,
                              bool reuseEnabled) {
    return reuseEnabled && errorFree && stored.codedWidth == wanted.codedWidth &&
        stored.codedHeight == wanted.codedHeight && stored.persistentBytes == wanted.persistentBytes &&
        stored.stagingBytes == wanted.stagingBytes && stored.bankBytes == wanted.bankBytes &&
        stored.profileIdc == wanted.profileIdc && stored.maxRefs == wanted.maxRefs;
}

inline DecodeFrameBuffers avcStreamBuffers(uint64_t stagingMc, uint64_t persistentMc,
                                         const AvcStreamGeometry &geometry, uint64_t dpbBankMc) {
    return {stagingMc + geometry.messageOffset, geometry.separateDpb ? dpbBankMc : persistentMc + geometry.dpbOffset,
            stagingMc + geometry.targetOffset, persistentMc,
            stagingMc + geometry.bitstreamOffset, persistentMc + geometry.contextOffset,
            stagingMc + geometry.feedbackOffset, stagingMc + geometry.scalingOffset};
}

inline bool buildAvcStreamMessage(uint32_t (&words)[kAvcStreamMessageDw],
                                  uint8_t (&scaling)[kAvcStreamScalingBytes],
                                  const AvcStreamSequence &sequence, const AvcStreamPicture &picture,
                                  uint32_t handle, uint32_t feedbackNumber, uint32_t bitstreamBytes) {
    AvcStreamGeometry geometry;
    if (!avcStreamGeometry(sequence, geometry) || !handle || !feedbackNumber ||
        bitstreamBytes < 5u || bitstreamBytes > kAvcStreamMaxBitstreamBytes) return false;
    const uint32_t frameMask = (1u << (sequence.log2MaxFrameNumMinus4 + 4u)) - 1u;
    if (picture.frameNum > frameMask || picture.referenceCount > sequence.maxNumRefFrames ||
        picture.currentSlot > sequence.maxNumRefFrames ||
        (picture.idr && (picture.frameNum || picture.referenceCount || picture.currentSlot)) ||
        (!picture.idr && !picture.referenceCount) ||
        picture.l0DefaultMinus1 >= 16 || picture.l1DefaultMinus1 >= 16 || picture.weightedBipredIdc > 2 ||
        picture.picInitQpMinus26 < -26 || picture.picInitQpMinus26 > 25 ||
        picture.chromaQpIndexOffset < -12 || picture.chromaQpIndexOffset > 12 ||
        picture.secondChromaQpIndexOffset < -12 || picture.secondChromaQpIndexOffset > 12 ||
        (sequence.profileIdc == 66 && (picture.entropyCoding || picture.weightedPred || picture.weightedBipredIdc)) ||
        (sequence.profileIdc != 100 && picture.transform8x8)) return false;
    uint32_t liveSlots = 1u << picture.currentSlot;
    for (uint32_t i = 0; i < picture.referenceCount; ++i) {
        const auto &ref = picture.references[i];
        if (ref.frameNum > frameMask || ref.slot > sequence.maxNumRefFrames || (liveSlots & (1u << ref.slot)))
            return false;
        liveSlots |= 1u << ref.slot;
    }

    // VCN 4 firmware packet from Mesa ac_vcn_dec.h. Compile-time bounds keep
    // the common header, indices and payloads within one cleared message page.
    constexpr uint32_t headerBytes = 40u, indicesBytes = headerBytes + 16u;
    constexpr uint32_t decodeBytes = 188u, codecOffset = indicesBytes + decodeBytes;
    constexpr uint32_t codecBytes = 1124u;
    static_assert(codecOffset + codecBytes == sizeof(words), "VCN AVC message layout");
    static_assert(sizeof(words) <= kAvcStreamAlignment, "VCN AVC message page");
    for (auto &word : words) word = 0;
    words[0] = headerBytes;
    words[1] = sizeof(words);
    words[2] = 2; // Generic decode and AVC submessages.
    words[3] = 1; // Render decode message.
    words[4] = handle;
    words[5] = feedbackNumber;
    words[6] = 2;
    words[7] = indicesBytes;
    words[8] = decodeBytes;
    words[10] = 6;
    words[11] = codecOffset;
    words[12] = codecBytes;

    uint32_t *decode = words + indicesBytes / sizeof(uint32_t);
    decode[0] = 7; // H.264 performance stream.
    decode[2] = sequence.codedWidth;
    decode[3] = sequence.codedHeight;
    decode[4] = avcStreamAlign(bitstreamBytes, 128u);
    decode[5] = geometry.dpbBytes;
    decode[6] = geometry.targetBytes;
    decode[9] = geometry.contextBytes;
    decode[10] = kDecodeSessionBytes;
    decode[18] = geometry.dpbPitch;
    decode[19] = geometry.dpbHeight;
    decode[22] = 3; // VCN 4.0.2 uses the GFX11 address library.
    decode[25] = geometry.pitch;
    decode[26] = geometry.pitch / 2u; // NV12 chroma samples each occupy two bytes.
    decode[29] = 3;
    decode[36] = geometry.lumaBytes;
    decode[37] = geometry.lumaBytes;
    decode[40] = 1; // MIF write control for VCN 3 and later.
    decode[41] = geometry.dpbChromaPitch;

    uint32_t *codec = words + codecOffset / sizeof(uint32_t);
    codec[0] = sequence.profileIdc == 100 ? 2u : sequence.profileIdc == 77 ? 1u : 0u;
    codec[1] = sequence.levelIdc;
    codec[2] = 0x84u | static_cast<uint32_t>(sequence.direct8x8Inference);
    codec[3] = static_cast<uint32_t>(picture.transform8x8) |
        (static_cast<uint32_t>(picture.constrainedIntraPred) << 2) |
        (static_cast<uint32_t>(picture.deblockingControlPresent) << 3) |
        (static_cast<uint32_t>(picture.weightedBipredIdc) << 4) |
        (static_cast<uint32_t>(picture.weightedPred) << 6) |
        (static_cast<uint32_t>(picture.bottomFieldPocPresent) << 7) |
        (static_cast<uint32_t>(picture.entropyCoding) << 8);
    codec[4] = 1u | (static_cast<uint32_t>(sequence.log2MaxFrameNumMinus4) << 24);
    codec[5] = sequence.picOrderCntType | (static_cast<uint32_t>(sequence.log2MaxPicOrderCntLsbMinus4) << 8) |
        (static_cast<uint32_t>(sequence.maxNumRefFrames) << 16);
    codec[6] = static_cast<uint8_t>(picture.picInitQpMinus26) |
        (static_cast<uint32_t>(static_cast<uint8_t>(picture.chromaQpIndexOffset)) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(picture.secondChromaQpIndexOffset)) << 24);
    codec[7] = (static_cast<uint32_t>(picture.l0DefaultMinus1) << 16) |
        (static_cast<uint32_t>(picture.l1DefaultMinus1) << 24);
    for (auto &value : scaling) value = 16u; // Limit the candidate to flat scaling matrices.
    for (uint32_t i = 0; i < sizeof(scaling) / sizeof(uint32_t); ++i) codec[9 + i] = 0x10101010u;

    codec[65] = picture.frameNum;
    codec[82] = static_cast<uint32_t>(picture.topPoc);
    codec[83] = static_cast<uint32_t>(picture.bottomPoc);
    // Mesa si_dec_h264 uses independent slots for decoded_pic_idx and ref_frame_list[].
    // frame_num_list[] and field_order_cnt_list[] are packed in the same reference order.
    codec[116] = picture.currentSlot;
    codec[117] = picture.referenceCount;
    for (uint32_t i = 118u; i < 122u; ++i) codec[i] = UINT32_MAX; // 16 absent slot IDs.
    auto *slots = reinterpret_cast<uint8_t *>(codec + 118);
    for (uint32_t i = 0; i < picture.referenceCount; ++i) {
        const auto &ref = picture.references[i];
        codec[66 + i] = ref.frameNum;
        codec[84 + i * 2] = static_cast<uint32_t>(ref.topPoc);
        codec[85 + i * 2] = static_cast<uint32_t>(ref.bottomPoc);
        slots[i] = ref.slot;
        codec[280] |= 3u << (2u * i); // Both fields of a progressive reference.
    }
    return true;
}

inline void initAvcStreamFeedback(uint32_t (&words)[kAvcStreamFeedbackDw]) {
    for (auto &word : words) word = 0;
    words[0] = sizeof(words);
    words[1] = sizeof(words);
}

inline bool avcStreamFeedbackOK(const uint32_t (&words)[kAvcStreamFeedbackDw], uint32_t feedbackNumber) {
    // Require an explicit report for this frame. The ring fence alone only
    // acknowledges retirement of the submitted IB.
    return feedbackNumber && words[0] == sizeof(words) && words[1] >= sizeof(words) &&
        words[1] <= kAvcStreamAlignment && words[3] == feedbackNumber && !words[4] && !words[6];
}

} // namespace phx_vcn

#endif
