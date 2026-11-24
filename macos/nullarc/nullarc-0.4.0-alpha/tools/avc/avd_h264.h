#ifndef PHOENIX_AVD_H264_H
#define PHOENIX_AVD_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id, profile, level, width, height, codedWidth, codedHeight;
    uint32_t cropLeft, cropTop;
    uint32_t chromaFormat, log2MaxFrameNumMinus4, picOrderCountType, log2MaxPicOrderCountLsbMinus4;
    uint32_t maxNumRefFrames;
    int32_t offsetForTopToBottomField;
    bool direct8x8Inference, gapsInFrameNumAllowed, deltaPicOrderAlwaysZero;
} PhoenixH264SPS;

typedef struct {
    uint32_t id, spsID, numRefIdxL0DefaultActiveMinus1, numRefIdxL1DefaultActiveMinus1, weightedBipredIDC;
    int32_t picInitQPMinus26, picInitQSMinus26, chromaQPIndexOffset, secondChromaQPIndexOffset;
    bool entropyCodingMode, bottomFieldPicOrderInFramePresent, weightedPred, deblockingFilterControlPresent;
    bool constrainedIntraPred, redundantPicCntPresent, transform8x8Mode;
} PhoenixH264PPS;

enum { PhoenixH264SliceP = 0, PhoenixH264SliceB = 1, PhoenixH264SliceI = 2 };
enum { PhoenixH264MaxShortTermReferences = 16 };

typedef struct {
    PhoenixH264SPS sps;
    PhoenixH264PPS pps;
    uint32_t sliceCount, frameNum, idrPicID;
    int32_t topFieldOrderCount, bottomFieldOrderCount;
    uint32_t pocLsb;
    int32_t bottomDelta;
    bool idr, noOutputOfPriorPics, longTermReference;
    // Contiguous Annex B slice NALs, including their original start codes.
    // Leading metadata, trailing NALs and trailing_zero_8bits are excluded.
    size_t vclOffset, vclLength;
    uint32_t sliceType; // Normalized H.264 slice_type (P, B or I).
    bool isReference, adaptiveRefPicMarking;
    uint32_t mmco1Count;
    uint32_t mmco1DifferenceMinus1[PhoenixH264MaxShortTermReferences];
} PhoenixH264AccessUnit;

// SPS and PPS readers take complete NAL units starting with the NAL header.
bool PhoenixH264ReadSPS(const uint8_t *nal, size_t size, PhoenixH264SPS *out);
bool PhoenixH264ReadPPS(const uint8_t *nal, size_t size, PhoenixH264PPS *out);

// Read one Annex B IDR access unit. Pass both out-of-band parameter sets or
// pass NULL/0 for each to use sets carried in the access unit.
// Supported syntax is progressive 8-bit 4:2:0, Baseline/Main/High, flat
// scaling lists and ordered intra slices without slice groups.
bool PhoenixH264ReadIDR(const uint8_t *annexB, size_t size,
                        const uint8_t *spsNAL, size_t spsSize,
                        const uint8_t *ppsNAL, size_t ppsSize,
                        PhoenixH264AccessUnit *out);

// Read a single-reference P picture using the preceding IDR's parameter sets.
// In-band repetitions must match the active decoding parameters.
// For type 0 POC, pocLsb and bottomDelta are raw syntax. Callers supply
// reference history to derive the final POC with wraparound.
bool PhoenixH264ReadP(const uint8_t *annexB, size_t size, const PhoenixH264SPS *sps,
                      const PhoenixH264PPS *pps, PhoenixH264AccessUnit *out);
// Read a P or B picture with short-term references. Explicit weights and
// per-slice reference list modifications remain in the VCL sent to the VCN.
bool PhoenixH264ReadInter(const uint8_t *annexB, size_t size, const PhoenixH264SPS *sps,
                          const PhoenixH264PPS *pps, PhoenixH264AccessUnit *out);
#ifdef __cplusplus
}
#endif

#endif
