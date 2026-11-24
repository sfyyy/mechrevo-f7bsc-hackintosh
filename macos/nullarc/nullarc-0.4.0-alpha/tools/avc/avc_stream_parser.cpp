// SPDX-License-Identifier: MIT
#include "avc_stream_parser.h"

namespace phx_vcn {

bool AvcStreamParser::parse(const uint8_t *data, size_t size, AvcStreamSequence &sequence,
                            AvcStreamPicture &picture, uint32_t &sliceCount, AvcVclRange *vcl) {
    if (size > kAvcStreamMaxBitstreamBytes) return false;
    PhoenixH264AccessUnit au{};
    if (!PhoenixH264ReadIDR(data, size, nullptr, 0, nullptr, 0, &au) &&
        (!haveReference_ || !PhoenixH264ReadInter(data, size, &sps_, &pps_, &au))) return false;
    const auto &s = au.sps;
    const auto &p = au.pps;
    // The linear target supports right/bottom cropping. The DPB tracks progressive short-term pictures.
    if (s.cropLeft || s.cropTop || s.gapsInFrameNumAllowed || au.longTermReference ||
        p.redundantPicCntPresent || p.numRefIdxL0DefaultActiveMinus1 >= 16 ||
        p.numRefIdxL1DefaultActiveMinus1 >= 16 || p.picInitQSMinus26 ||
        (s.profile == 66 && (p.entropyCodingMode || p.weightedPred || p.weightedBipredIDC || au.sliceType == PhoenixH264SliceB)) ||
        (s.profile != 100 && p.transform8x8Mode)) return false;
    AvcStreamSequence seq{s.codedWidth, s.codedHeight, s.width, s.height,
        static_cast<uint8_t>(s.profile), static_cast<uint8_t>(s.level), static_cast<uint8_t>(s.log2MaxFrameNumMinus4),
        static_cast<uint8_t>(s.picOrderCountType), static_cast<uint8_t>(s.log2MaxPicOrderCountLsbMinus4),
        static_cast<uint8_t>(s.maxNumRefFrames), s.direct8x8Inference};
    AvcStreamGeometry geometry;
    if (!avcStreamGeometry(seq, geometry)) return false;

    AvcStreamPicture pic{};
    pic.idr = au.idr;
    pic.frameNum = au.frameNum;
    pic.weightedBipredIdc = static_cast<uint8_t>(p.weightedBipredIDC);
    pic.l0DefaultMinus1 = static_cast<uint8_t>(p.numRefIdxL0DefaultActiveMinus1);
    pic.l1DefaultMinus1 = static_cast<uint8_t>(p.numRefIdxL1DefaultActiveMinus1);
    pic.entropyCoding = p.entropyCodingMode;
    pic.bottomFieldPocPresent = p.bottomFieldPicOrderInFramePresent;
    pic.deblockingControlPresent = p.deblockingFilterControlPresent;
    pic.constrainedIntraPred = p.constrainedIntraPred;
    pic.weightedPred = p.weightedPred;
    pic.transform8x8 = p.transform8x8Mode;
    pic.picInitQpMinus26 = static_cast<int8_t>(p.picInitQPMinus26);
    pic.chromaQpIndexOffset = static_cast<int8_t>(p.chromaQPIndexOffset);
    pic.secondChromaQpIndexOffset = static_cast<int8_t>(p.secondChromaQPIndexOffset);
    int64_t top = au.topFieldOrderCount, bottom = au.bottomFieldOrderCount;
    const uint32_t frameMask = (1u << (s.log2MaxFrameNumMinus4 + 4)) - 1;
    if (!pic.idr) {
        if (!referenceCount_ || (au.isReference && au.frameNum != ((previousReference_.frameNum + 1) & frameMask))) return false;
        if (s.picOrderCountType == 2) {
            const uint32_t distance = (au.frameNum - previousReference_.frameNum) & frameMask;
            top = bottom = static_cast<int64_t>(previousReference_.topPoc) + 2 * distance - (au.isReference ? 0 : 1);
        } else {
            int32_t range = 1 << (s.log2MaxPicOrderCountLsbMinus4 + 4);
            int32_t previousLsb = previousReference_.topPoc & (range - 1);
            int32_t lsb = static_cast<int32_t>(au.pocLsb);
            int64_t msb = static_cast<int64_t>(previousReference_.topPoc) - previousLsb;
            if (lsb < previousLsb && previousLsb - lsb >= range / 2) msb += range;
            if (lsb > previousLsb && lsb - previousLsb > range / 2) msb -= range;
            top = msb + lsb;
            bottom = top + au.bottomDelta;
        }
    } else if (s.picOrderCountType == 0) {
        // On an IDR, previous PicOrderCntMsb/Lsb default to zero (H.264 8.2.1.1).
        int32_t range = 1 << (s.log2MaxPicOrderCountLsbMinus4 + 4);
        if (top > range / 2) {
            top -= range;
            bottom -= range;
        }
    }
    if (top < INT32_MIN || top > INT32_MAX || bottom < INT32_MIN || bottom > INT32_MAX) return false;
    pic.topPoc = static_cast<int32_t>(top);
    pic.bottomPoc = static_cast<int32_t>(bottom);

    // Supply the DPB as it stood before the current picture. A non-reference B can
    // reuse the same free surface on successive calls, independently of frame_num.
    pic.referenceCount = pic.idr ? 0 : referenceCount_;
    uint32_t occupied = 0;
    for (uint32_t i = 0; i < pic.referenceCount; ++i) {
        pic.references[i] = references_[i];
        occupied |= 1u << references_[i].slot;
    }
    while (occupied & (1u << pic.currentSlot)) ++pic.currentSlot;
    if (pic.currentSlot > s.maxNumRefFrames) return false;
    const AvcReference current{pic.frameNum, pic.topPoc, pic.bottomPoc, pic.currentSlot};

    // MMCO 1 names a short-term PicNum relative to this frame. Apply removals
    // only after all targets validate, preserving parser state on a rejected AU.
    uint32_t removed = 0;
    if (au.adaptiveRefPicMarking) {
        for (uint32_t op = 0; op < au.mmco1Count; ++op) {
            const uint32_t deltaMinus1 = au.mmco1DifferenceMinus1[op];
            if (deltaMinus1 >= frameMask) return false;
            const uint32_t target = (pic.frameNum - (deltaMinus1 + 1u)) & frameMask;
            uint32_t i = 0;
            while (i < pic.referenceCount && references_[i].frameNum != target) ++i;
            if (i == pic.referenceCount || (removed & (1u << i))) return false;
            removed |= 1u << i;
        }
    } else if (au.isReference && pic.referenceCount && pic.referenceCount == s.maxNumRefFrames) {
        // With no gaps or long-term pictures, insertion order equals increasing FrameNumWrap.
        removed = 1;
    }
    AvcReference updated[kAvcMaxReferences]{};
    uint8_t retained = 0;
    for (uint32_t i = 0; i < pic.referenceCount; ++i)
        if (!(removed & (1u << i))) updated[retained++] = references_[i];
    if (au.isReference && s.maxNumRefFrames) {
        if (retained >= s.maxNumRefFrames) return false;
        updated[retained++] = current;
    }
    for (uint32_t i = 0; i < retained; ++i) references_[i] = updated[i];
    referenceCount_ = retained;
    sps_ = s;
    pps_ = p;
    if (au.isReference) previousReference_ = current;
    haveReference_ = retained != 0;
    sequence = seq;
    picture = pic;
    sliceCount = au.sliceCount;
    if (vcl) *vcl = {au.vclOffset, au.vclLength};
    return true;
}

} // namespace phx_vcn
