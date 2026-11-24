#include "avd_h264.h"

#include <string.h>

typedef struct {
    const uint8_t *next, *end;
    unsigned zeros, remaining;
    uint8_t byte;
    bool failed;
} RBSP;

static RBSP rbsp(const uint8_t *nal, size_t size, uint8_t type) {
    if (!nal || size < 2 || (nal[0] & 0x9f) != type || (type != 1 && !(nal[0] & 0x60)))
        return (RBSP){.failed = true};
    return (RBSP){.next = nal + 1, .end = nal + size};
}

static uint32_t bits(RBSP *r, unsigned count) {
    uint32_t value = 0;
    while (count-- && !r->failed) {
        if (!r->remaining) {
            if (r->next == r->end) {
                r->failed = true;
                break;
            }
            uint8_t byte = *r->next++;
            if (r->zeros == 2 && byte <= 3) {
                if (byte != 3 || r->next == r->end || *r->next > 3) {
                    r->failed = true;
                    break;
                }
                byte = *r->next++;
                r->zeros = 0;
            }
            r->zeros = byte ? 0 : r->zeros + 1;
            r->byte = byte;
            r->remaining = 8;
        }
        value = (value << 1) | ((r->byte >> --r->remaining) & 1u);
    }
    return value;
}

static uint32_t ue(RBSP *r, uint32_t max) {
    unsigned zeros = 0;
    while (!bits(r, 1) && !r->failed) {
        if (++zeros == 32) r->failed = true;
    }
    if (r->failed) return 0;
    uint32_t value = ((UINT32_C(1) << zeros) - 1) + bits(r, zeros);
    if (value > max) r->failed = true;
    return r->failed ? 0 : value;
}

static int32_t se(RBSP *r, int32_t min, int32_t max) {
    uint32_t v = ue(r, UINT32_MAX - 1);
    int32_t value = (v & 1) ? (int32_t)(v / 2 + 1) : -(int32_t)(v / 2);
    if (value < min || value > max) r->failed = true;
    return value;
}

static bool trailingBits(RBSP r) {
    if (bits(&r, 1) != 1) return false;
    if (bits(&r, r.remaining) != 0) return false;
    return !r.failed && r.next == r.end;
}

static void skipHRD(RBSP *r) {
    uint32_t count = ue(r, 31) + 1;
    bits(r, 8);
    for (uint32_t i = 0; i < count && !r->failed; ++i) {
        ue(r, UINT32_MAX - 1);
        ue(r, UINT32_MAX - 1);
        bits(r, 1);
    }
    bits(r, 20);
}

static void skipVUI(RBSP *r) {
    if (bits(r, 1) && bits(r, 8) == 255) bits(r, 32);
    if (bits(r, 1)) bits(r, 1);
    if (bits(r, 1)) {
        bits(r, 4);
        if (bits(r, 1)) bits(r, 24);
    }
    if (bits(r, 1)) {
        ue(r, 5);
        ue(r, 5);
    }
    if (bits(r, 1)) {
        bits(r, 32);
        bits(r, 32);
        bits(r, 1);
    }
    bool nalHRD = bits(r, 1);
    if (nalHRD) skipHRD(r);
    bool vclHRD = bits(r, 1);
    if (vclHRD) skipHRD(r);
    if (nalHRD || vclHRD) bits(r, 1);
    bits(r, 1);
    if (bits(r, 1)) {
        bits(r, 1);
        ue(r, 16);
        ue(r, 16);
        ue(r, 16);
        ue(r, 16);
        ue(r, 16);
        ue(r, 16);
    }
}

bool PhoenixH264ReadSPS(const uint8_t *nal, size_t size, PhoenixH264SPS *out) {
    if (!out) return false;
    RBSP r = rbsp(nal, size, 7);
    PhoenixH264SPS sps = {.chromaFormat = 1};
    sps.profile = bits(&r, 8);
    uint32_t constraints = bits(&r, 8);
    sps.level = bits(&r, 8);
    sps.id = ue(&r, 31);
    if (r.failed || (constraints & 3) || !sps.level) return false;
    if (sps.profile == 100) {
        sps.chromaFormat = ue(&r, 3);
        if (sps.chromaFormat != 1 || ue(&r, 6) || ue(&r, 6) || bits(&r, 1) || bits(&r, 1))
            return false;
    } else if (sps.profile != 66 && sps.profile != 77) {
        return false;
    }

    sps.log2MaxFrameNumMinus4 = ue(&r, 12);
    sps.picOrderCountType = ue(&r, 2);
    if (sps.picOrderCountType == 0) {
        sps.log2MaxPicOrderCountLsbMinus4 = ue(&r, 12);
    } else if (sps.picOrderCountType == 1) {
        sps.deltaPicOrderAlwaysZero = bits(&r, 1);
        se(&r, INT32_MIN, INT32_MAX);
        sps.offsetForTopToBottomField = se(&r, INT32_MIN, INT32_MAX);
        uint32_t count = ue(&r, 255);
        for (uint32_t i = 0; i < count && !r.failed; ++i) se(&r, INT32_MIN, INT32_MAX);
    }
    sps.maxNumRefFrames = ue(&r, 16);
    sps.gapsInFrameNumAllowed = bits(&r, 1);
    sps.codedWidth = (ue(&r, UINT16_MAX / 16 - 1) + 1) * 16;
    sps.codedHeight = (ue(&r, UINT16_MAX / 16 - 1) + 1) * 16;
    if (!bits(&r, 1)) return false; // frame_mbs_only_flag
    sps.direct8x8Inference = bits(&r, 1);
    sps.width = sps.codedWidth;
    sps.height = sps.codedHeight;
    if (bits(&r, 1)) {
        sps.cropLeft = 2 * ue(&r, sps.codedWidth / 2 - 1);
        uint32_t cropRight = 2 * ue(&r, sps.codedWidth / 2 - 1);
        sps.cropTop = 2 * ue(&r, sps.codedHeight / 2 - 1);
        uint32_t cropBottom = 2 * ue(&r, sps.codedHeight / 2 - 1);
        if (sps.cropLeft + cropRight >= sps.codedWidth || sps.cropTop + cropBottom >= sps.codedHeight)
            return false;
        sps.width -= sps.cropLeft + cropRight;
        sps.height -= sps.cropTop + cropBottom;
    }
    if (bits(&r, 1)) skipVUI(&r);
    if (!trailingBits(r)) return false;
    *out = sps;
    return true;
}

bool PhoenixH264ReadPPS(const uint8_t *nal, size_t size, PhoenixH264PPS *out) {
    if (!out) return false;
    RBSP r = rbsp(nal, size, 8);
    PhoenixH264PPS pps = {0};
    pps.id = ue(&r, 255);
    pps.spsID = ue(&r, 31);
    pps.entropyCodingMode = bits(&r, 1);
    pps.bottomFieldPicOrderInFramePresent = bits(&r, 1);
    if (ue(&r, 7)) return false; // num_slice_groups_minus1
    pps.numRefIdxL0DefaultActiveMinus1 = ue(&r, 31);
    pps.numRefIdxL1DefaultActiveMinus1 = ue(&r, 31);
    pps.weightedPred = bits(&r, 1);
    pps.weightedBipredIDC = bits(&r, 2);
    if (pps.weightedBipredIDC > 2) return false;
    pps.picInitQPMinus26 = se(&r, -26, 25);
    pps.picInitQSMinus26 = se(&r, -26, 25);
    pps.chromaQPIndexOffset = se(&r, -12, 12);
    pps.deblockingFilterControlPresent = bits(&r, 1);
    pps.constrainedIntraPred = bits(&r, 1);
    pps.redundantPicCntPresent = bits(&r, 1);
    pps.secondChromaQPIndexOffset = pps.chromaQPIndexOffset;
    if (!trailingBits(r)) {
        pps.transform8x8Mode = bits(&r, 1);
        if (bits(&r, 1)) return false; // pic_scaling_matrix_present_flag
        pps.secondChromaQPIndexOffset = se(&r, -12, 12);
    }
    if (!trailingBits(r)) return false;
    *out = pps;
    return true;
}

typedef struct {
    uint32_t firstMB, frameNum, idrPicID, sliceType;
    int32_t top, bottom, bottomDelta;
    bool noOutput, longTerm, isReference, adaptiveRefPicMarking;
    uint32_t mmco1Count, mmco1DifferenceMinus1[PhoenixH264MaxShortTermReferences];
} SliceHeader;

static bool readListModifications(RBSP *r, uint32_t activeMinus1, uint32_t maxPicNumMinus1, bool legacyP) {
    if (!bits(r, 1)) return !r->failed;
    if (legacyP) return false;
    for (uint32_t i = 0; !r->failed; ++i) {
        uint32_t idc = ue(r, 3);
        if (idc == 3) return true;
        if (idc > 1 || i > activeMinus1) return false;
        ue(r, maxPicNumMinus1); // abs_diff_pic_num_minus1
    }
    return false;
}

static void readWeights(RBSP *r, uint32_t activeMinus1) {
    for (uint32_t i = 0; i <= activeMinus1 && !r->failed; ++i) {
        if (bits(r, 1)) {
            se(r, -128, 127); // luma weight and offset
            se(r, -128, 127);
        }
        if (bits(r, 1)) {
            for (unsigned j = 0; j < 2; ++j) {
                se(r, -128, 127); // Cb/Cr weight and offset
                se(r, -128, 127);
            }
        }
    }
}

static bool readInterPrediction(RBSP *r, const PhoenixH264PPS *pps, bool bSlice,
                                uint32_t maxPicNumMinus1, bool legacyP) {
    uint32_t l0 = pps->numRefIdxL0DefaultActiveMinus1, l1 = pps->numRefIdxL1DefaultActiveMinus1;
    if (bSlice) bits(r, 1); // direct_spatial_mv_pred_flag
    if (bits(r, 1)) {
        l0 = ue(r, 15);
        if (bSlice) l1 = ue(r, 15);
    }
    if (l0 > 15 || (bSlice && l1 > 15) || (legacyP && (bSlice || l0 || pps->weightedPred))) return false;
    if (!readListModifications(r, l0, maxPicNumMinus1, legacyP) ||
        (bSlice && !readListModifications(r, l1, maxPicNumMinus1, legacyP))) return false;
    if ((!bSlice && pps->weightedPred) || (bSlice && pps->weightedBipredIDC == 1)) {
        ue(r, 7); // luma_log2_weight_denom
        ue(r, 7); // chroma_log2_weight_denom (4:2:0)
        readWeights(r, l0);
        if (bSlice) readWeights(r, l1);
    }
    return !r->failed;
}

static bool readShortTermMarking(RBSP *r, uint32_t maxPicNumMinus1, uint32_t maxRefs,
                                 bool legacyP, SliceHeader *slice) {
    slice->adaptiveRefPicMarking = bits(r, 1);
    if (!slice->adaptiveRefPicMarking) return !r->failed;
    if (legacyP) return false;
    while (!r->failed) {
        uint32_t operation = ue(r, 6);
        if (!operation) return !r->failed;
        if (operation != 1 || slice->mmco1Count == maxRefs ||
            slice->mmco1Count == PhoenixH264MaxShortTermReferences) return false;
        slice->mmco1DifferenceMinus1[slice->mmco1Count++] = ue(r, maxPicNumMinus1);
    }
    return false;
}

static bool readSlice(const uint8_t *nal, size_t size, const PhoenixH264AccessUnit *picture,
                      bool legacyP, SliceHeader *out) {
    const bool idr = picture->idr;
    RBSP r = rbsp(nal, size, idr ? 5 : 1);
    const PhoenixH264SPS *sps = &picture->sps;
    const PhoenixH264PPS *pps = &picture->pps;
    SliceHeader slice = {0};
    uint32_t macroblocks = (sps->codedWidth / 16) * (sps->codedHeight / 16);
    slice.firstMB = ue(&r, macroblocks - 1);
    slice.sliceType = ue(&r, 9) % 5;
    slice.isReference = (nal[0] & 0x60) != 0;
    if ((idr && slice.sliceType != PhoenixH264SliceI) || (!idr && slice.sliceType > PhoenixH264SliceB) ||
        (legacyP && !slice.isReference) || ue(&r, 255) != pps->id) return false;
    slice.frameNum = bits(&r, sps->log2MaxFrameNumMinus4 + 4);
    if (idr) {
        if (slice.frameNum) return false;
        slice.idrPicID = ue(&r, 65535);
    }
    int64_t bottom = 0;
    if (sps->picOrderCountType == 0) {
        slice.top = (int32_t)bits(&r, sps->log2MaxPicOrderCountLsbMinus4 + 4);
        bottom = slice.top;
        if (pps->bottomFieldPicOrderInFramePresent) slice.bottomDelta = se(&r, INT32_MIN, INT32_MAX);
        bottom += slice.bottomDelta;
    } else if (sps->picOrderCountType == 1) {
        if (!sps->deltaPicOrderAlwaysZero) slice.top = se(&r, INT32_MIN, INT32_MAX);
        bottom = (int64_t)slice.top + sps->offsetForTopToBottomField;
        if (!sps->deltaPicOrderAlwaysZero && pps->bottomFieldPicOrderInFramePresent)
            bottom += se(&r, INT32_MIN, INT32_MAX);
    }
    if (bottom < INT32_MIN || bottom > INT32_MAX) return false;
    slice.bottom = (int32_t)bottom;
    if (pps->redundantPicCntPresent && ue(&r, 127)) return false;
    if (idr) {
        slice.noOutput = bits(&r, 1);
        slice.longTerm = bits(&r, 1);
    } else {
        uint32_t maxPicNumMinus1 = (1u << (sps->log2MaxFrameNumMinus4 + 4)) - 1;
        if (!readInterPrediction(&r, pps, slice.sliceType == PhoenixH264SliceB, maxPicNumMinus1, legacyP))
            return false;
        if (slice.isReference && !readShortTermMarking(&r, maxPicNumMinus1, sps->maxNumRefFrames, legacyP, &slice))
            return false;
        if (pps->entropyCodingMode) ue(&r, 2); // cabac_init_idc
    }
    int32_t qp = 26 + pps->picInitQPMinus26 + se(&r, -51, 51);
    if (qp < 0 || qp > 51) return false;
    if (pps->deblockingFilterControlPresent && ue(&r, 2) != 1) {
        se(&r, -6, 6);
        se(&r, -6, 6);
    }
    if (pps->entropyCodingMode) {
        while (r.remaining && !r.failed)
            if (bits(&r, 1) != 1) return false;
    }
    if (r.failed || trailingBits(r) || (!r.remaining && r.next == r.end)) return false;
    *out = slice;
    return true;
}

static bool findStartCode(const uint8_t *data, size_t length, size_t *start, size_t *payload) {
    for (size_t i = *start; i < length && length - i >= 3; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *start = i > *start && data[i - 1] == 0 ? i - 1 : i;
            *payload = i + 3;
            return true;
        }
    }
    return false;
}

static bool sameSPS(const PhoenixH264SPS *a, const PhoenixH264SPS *b) {
    return a->id == b->id && a->profile == b->profile && a->level == b->level &&
        a->width == b->width && a->height == b->height &&
        a->codedWidth == b->codedWidth && a->codedHeight == b->codedHeight &&
        a->cropLeft == b->cropLeft && a->cropTop == b->cropTop && a->chromaFormat == b->chromaFormat &&
        a->log2MaxFrameNumMinus4 == b->log2MaxFrameNumMinus4 && a->picOrderCountType == b->picOrderCountType &&
        a->log2MaxPicOrderCountLsbMinus4 == b->log2MaxPicOrderCountLsbMinus4 &&
        a->maxNumRefFrames == b->maxNumRefFrames && a->offsetForTopToBottomField == b->offsetForTopToBottomField &&
        a->direct8x8Inference == b->direct8x8Inference && a->gapsInFrameNumAllowed == b->gapsInFrameNumAllowed &&
        a->deltaPicOrderAlwaysZero == b->deltaPicOrderAlwaysZero;
}

static bool samePPS(const PhoenixH264PPS *a, const PhoenixH264PPS *b) {
    return a->id == b->id && a->spsID == b->spsID &&
        a->numRefIdxL0DefaultActiveMinus1 == b->numRefIdxL0DefaultActiveMinus1 &&
        a->numRefIdxL1DefaultActiveMinus1 == b->numRefIdxL1DefaultActiveMinus1 &&
        a->weightedBipredIDC == b->weightedBipredIDC && a->picInitQPMinus26 == b->picInitQPMinus26 &&
        a->picInitQSMinus26 == b->picInitQSMinus26 && a->chromaQPIndexOffset == b->chromaQPIndexOffset &&
        a->secondChromaQPIndexOffset == b->secondChromaQPIndexOffset && a->entropyCodingMode == b->entropyCodingMode &&
        a->bottomFieldPicOrderInFramePresent == b->bottomFieldPicOrderInFramePresent && a->weightedPred == b->weightedPred &&
        a->deblockingFilterControlPresent == b->deblockingFilterControlPresent &&
        a->constrainedIntraPred == b->constrainedIntraPred && a->redundantPicCntPresent == b->redundantPicCntPresent &&
        a->transform8x8Mode == b->transform8x8Mode;
}

static bool readAU(const uint8_t *annexB, size_t size,
                   const uint8_t *spsNAL, size_t spsSize,
                   const uint8_t *ppsNAL, size_t ppsSize,
                   const PhoenixH264SPS *sequence, const PhoenixH264PPS *parameters,
                   bool legacyP, PhoenixH264AccessUnit *out) {
    if (!annexB || !out || (!!spsNAL != !!spsSize) || (!!ppsNAL != !!ppsSize) || (!!spsNAL != !!ppsNAL))
        return false;
    size_t prefix = 0, pos;
    if (!findStartCode(annexB, size, &prefix, &pos) || pos == size) return false;
    for (size_t i = 0; i < prefix; ++i)
        if (annexB[i]) return false;
    PhoenixH264AccessUnit picture = {.idr = !sequence};
    if (sequence) {
        picture.sps = *sequence;
        picture.pps = *parameters;
    }
    uint32_t previousMB = 0;
    bool firstNAL = true, vclEnded = false;
    while (pos < size) {
        size_t start = prefix;
        const uint8_t *nal = annexB + pos;
        size_t next = pos, nextPayload = 0;
        if (findStartCode(annexB, size, &next, &nextPayload)) {
            if (nextPayload == size) return false;
        } else {
            next = nextPayload = size;
        }
        prefix = next;
        while (next > pos && !annexB[next - 1]) --next;
        size_t bytes = next - pos;
        pos = nextPayload;
        if (!bytes || (nal[0] & 0x80)) return false;
        uint8_t type = nal[0] & 31;
        if (type == 7 || type == 8) {
            if (picture.sliceCount) return false;
            if (!picture.idr) {
                if (type == 7) {
                    PhoenixH264SPS repeated;
                    if (!PhoenixH264ReadSPS(nal, bytes, &repeated) || !sameSPS(&repeated, sequence)) return false;
                } else {
                    PhoenixH264PPS repeated;
                    if (!PhoenixH264ReadPPS(nal, bytes, &repeated) || !samePPS(&repeated, parameters)) return false;
                }
            } else {
                const uint8_t **set = type == 7 ? &spsNAL : &ppsNAL;
                size_t *setSize = type == 7 ? &spsSize : &ppsSize;
                if (*set && (*setSize != bytes || memcmp(*set, nal, bytes))) return false;
                *set = nal;
                *setSize = bytes;
            }
        } else if (type == (picture.idr ? 5 : 1)) {
            if (vclEnded) return false;
            SliceHeader slice;
            if (picture.idr && !picture.sliceCount &&
                (!PhoenixH264ReadSPS(spsNAL, spsSize, &picture.sps) ||
                 !PhoenixH264ReadPPS(ppsNAL, ppsSize, &picture.pps) ||
                 picture.pps.spsID != picture.sps.id)) return false;
            if (!readSlice(nal, bytes, &picture, legacyP, &slice)) return false;
            if (!picture.sliceCount) {
                if (slice.firstMB) return false;
                picture.vclOffset = start;
                picture.frameNum = slice.frameNum;
                picture.idrPicID = slice.idrPicID;
                picture.topFieldOrderCount = slice.top;
                picture.bottomFieldOrderCount = slice.bottom;
                if (picture.sps.picOrderCountType == 0) picture.pocLsb = (uint32_t)slice.top;
                picture.bottomDelta = slice.bottomDelta;
                picture.noOutputOfPriorPics = slice.noOutput;
                picture.longTermReference = slice.longTerm;
                picture.sliceType = slice.sliceType;
                picture.isReference = slice.isReference;
                picture.adaptiveRefPicMarking = slice.adaptiveRefPicMarking;
                picture.mmco1Count = slice.mmco1Count;
                memcpy(picture.mmco1DifferenceMinus1, slice.mmco1DifferenceMinus1, slice.mmco1Count * sizeof(uint32_t));
            } else if (slice.firstMB <= previousMB || slice.frameNum != picture.frameNum || slice.idrPicID != picture.idrPicID ||
                       slice.top != picture.topFieldOrderCount || slice.bottom != picture.bottomFieldOrderCount ||
                       slice.noOutput != picture.noOutputOfPriorPics || slice.longTerm != picture.longTermReference ||
                       slice.sliceType != picture.sliceType || slice.isReference != picture.isReference ||
                       slice.adaptiveRefPicMarking != picture.adaptiveRefPicMarking || slice.mmco1Count != picture.mmco1Count ||
                       memcmp(slice.mmco1DifferenceMinus1, picture.mmco1DifferenceMinus1, slice.mmco1Count * sizeof(uint32_t))) {
                return false;
            }
            previousMB = slice.firstMB;
            picture.vclLength = next - picture.vclOffset;
            ++picture.sliceCount;
        } else if (type == 9) {
            if (!firstNAL) return false;
        } else if (type == 6 || type == 12 || ((type == 10 || type == 11) && picture.sliceCount)) {
            if (picture.sliceCount) vclEnded = true;
        } else {
            return false;
        }
        firstNAL = false;
    }
    if (!picture.sliceCount) return false;
    *out = picture;
    return true;
}

bool PhoenixH264ReadIDR(const uint8_t *annexB, size_t size,
                        const uint8_t *spsNAL, size_t spsSize,
                        const uint8_t *ppsNAL, size_t ppsSize,
                        PhoenixH264AccessUnit *out) {
    return readAU(annexB, size, spsNAL, spsSize, ppsNAL, ppsSize, NULL, NULL, false, out);
}

static bool readInter(const uint8_t *annexB, size_t size, const PhoenixH264SPS *sps,
                      const PhoenixH264PPS *pps, bool legacyP, PhoenixH264AccessUnit *out) {
    // Only parameter sets produced by the readers above are valid for this API.
    if (!sps || !pps || sps->id != pps->spsID || sps->log2MaxFrameNumMinus4 > 12 ||
        sps->log2MaxPicOrderCountLsbMinus4 > 12 || (sps->picOrderCountType != 0 && sps->picOrderCountType != 2) ||
        sps->codedWidth < 16 || sps->codedWidth > UINT16_MAX || sps->codedHeight < 16 ||
        sps->codedHeight > UINT16_MAX || pps->picInitQPMinus26 < -26 || pps->picInitQPMinus26 > 25) return false;
    return readAU(annexB, size, NULL, 0, NULL, 0, sps, pps, legacyP, out);
}

bool PhoenixH264ReadP(const uint8_t *annexB, size_t size, const PhoenixH264SPS *sps,
                      const PhoenixH264PPS *pps, PhoenixH264AccessUnit *out) {
    return readInter(annexB, size, sps, pps, true, out);
}

bool PhoenixH264ReadInter(const uint8_t *annexB, size_t size, const PhoenixH264SPS *sps,
                          const PhoenixH264PPS *pps, PhoenixH264AccessUnit *out) {
    return readInter(annexB, size, sps, pps, false, out);
}
