#include "avd_decode.h"
#include "avc_parameters.h"
#include "avd_h264.h"
#include "PhxVideoDecode.h"
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#if PHX_REFERENCE_DECODE_PROBE

static int referenceResult(const char *phase, int status) {
    os_log(OS_LOG_DEFAULT, "Phoenix reference IDR diagnostic phase=%{public}s status=%{public}d", phase, status);
    return status;
}
#else
#define referenceResult(phase, status) (status)
#endif

enum { NotSupported = 1, BadParameter = 5, NoMemory = 7, SliceCount = 5,
       MaxStreamSlices = (PHX_VIDEO_DECODE_STREAM_MAX_WIDTH / 16) * (PHX_VIDEO_DECODE_STREAM_MAX_HEIGHT / 16) };

_Static_assert(offsetof(PhoenixAVDCommand, next) == 0x30, "linked output command");
_Static_assert(offsetof(PhoenixAVDDecodePicture, sps) == 0x260 &&
               offsetof(PhoenixAVDDecodePicture, poolID) == 0x270 &&
               offsetof(PhoenixAVDDecodePicture, sliceCount) == 0x278 &&
               offsetof(PhoenixAVDDecodePicture, slices) == 0x290, "AMD2 AVC picture");
_Static_assert(offsetof(PhoenixAVDDecodeTarget, surface) == 0x10, "output IOSurface");

// AppleGVA copies these complete, zero-initialized parameter blocks into each
// AVC command. These values were obtained by running its x86_64 AVC parser on
// the libdrm IDR's SPS 6764001eac5230360f64 and PPS 68de09c8. Other syntax
// (including scaling lists or cropping) must not reach the fixed VCN message.
static const uint32_t referenceSPS[0xc18 / 4] = {
    [0] = 1, [0x004/4] = 100, [0x024/4] = 30, [0x02c/4] = 1,
    [0x404/4] = 1, [0x40c/4] = 3, [0x820/4] = 2, [0x828/4] = 53,
    [0x82c/4] = 29, [0x830/4] = 1, [0x838/4] = 1,
};
static const uint32_t referencePPS[0x488 / 4] = {
    [0] = 1, [0x010/4] = 1, [0x0a0/4] = 2, [0x0ac/4] = 1,
};

// AppleGVA submits an ordered list of slice NALs. The first slice starts at
// macroblock zero (ue(v) = 1). Its next ue(v), slice_type, occupies at most
// seven bits for the valid range 0..9, entirely in this first RBSP byte.
static unsigned firstSliceKind(const uint8_t *slices, size_t size, unsigned *nal, unsigned *ref) {
    size_t i = 0;
    while (i < size && !slices[i]) ++i;
    if ((i != 2 && i != 3) || size - i < 3 || slices[i] != 1) return 6;
    unsigned header = slices[i + 1], bits = slices[i + 2];
    *nal = header & 31;
    *ref = (header >> 5) & 3;
    if ((header & 0x80) || (*nal != 1 && *nal != 5) || !(bits & 0x80)) return 6;
    for (unsigned zeros = 0; zeros <= 3; ++zeros) {
        if (!(bits & (1u << (6 - zeros)))) continue;
        unsigned type = (1u << zeros) - 1 + ((bits >> (6 - 2 * zeros)) & ((1u << zeros) - 1));
        return type <= 9 ? type % 5 : 6;
    }
    return 6;
}

enum RejectionStage { ExportFailure, SPSFailure, SubmitFailure };

static int reportRejection(uint32_t *reported, enum RejectionStage stage,
                           const PhoenixAVDDecodePicture *picture, const uint8_t *slices,
                           size_t sliceBytes, const uint8_t *au, size_t bytes, OSStatus status) {
    unsigned nal = 0, ref = 0;
    unsigned kind = firstSliceKind(slices, sliceBytes, &nal, &ref);
    unsigned key = nal == 5 ? 5 : kind;
    uint32_t bit = 1u << (stage * 7u + key);
    if (*reported & bit) return NotSupported;
    *reported |= bit;
    // These block offsets use the same pinned AppleGVA layout as the exporter.
    if (!PhoenixAVDCompatibleAppleGVA()) return NotSupported;
    const uint32_t *s = picture->sps, *p = picture->pps;
    PhoenixH264AccessUnit parsed;
    int parsedIDR = (au && nal == 5) ? (int)PhoenixH264ReadIDR(au, bytes, NULL, 0, NULL, 0, &parsed) : -1;
    const char *phase = stage == ExportFailure ? "parameters" : stage == SPSFailure ? "sps" : "submit";
    os_log(OS_LOG_DEFAULT, "Phoenix AVC rejected phase=%{public}s status=%{public}d nal=%{public}u nal_ref_idc=%{public}u slice_type_mod5=%{public}u slices=%{public}u parsed_idr=%{public}d profile=%{public}u level=%{public}u max_refs=%{public}u poc_type=%{public}u gaps=%{public}u mb_minus1=%{public}ux%{public}u crop_units_lrtb=%{public}u,%{public}u,%{public}u,%{public}u cabac=%{public}u l0_minus1=%{public}u l1_minus1=%{public}u weighted_pred=%{public}u weighted_bipred=%{public}u",
           phase, (int)status, nal, ref, kind, picture->sliceCount, parsedIDR,
           s[4/4], s[0x24/4], s[0x820/4], s[0x408/4], s[0x824/4], s[0x828/4], s[0x82c/4],
           s[0x840/4], s[0x844/4], s[0x848/4], s[0x84c/4],
           p[0xc/4], p[0x90/4], p[0x94/4], p[0x98/4], p[0x9c/4]);
    return NotSupported;
}

static int submitStream(PhoenixVADecoder *decoder, uint32_t *reported, const PhoenixAVDDecodePicture *picture,
                        const uint8_t *slices, size_t sliceBytes, CVPixelBufferRef pixel) {
    uint8_t parameters[PhoenixAVCParameterCapacity];
    size_t parameterBytes = 0;
    if (!PhoenixAVDExportAVCParameters(picture->sps, picture->pps, parameters, sizeof(parameters), &parameterBytes) ||
        parameterBytes > sizeof(parameters) || sliceBytes > PHX_VIDEO_DECODE_STREAM_MAX_INPUT - parameterBytes)
        return reportRejection(reported, ExportFailure, picture, slices, sliceBytes, NULL, 0, NotSupported);

    // The exporter prefixes both parameter NALs with four-byte start codes.
    // Derive the visible output size from the SPS carried to the kernel.
    const uint8_t prefix[] = {0, 0, 0, 1};
    size_t ppsOffset = sizeof(prefix);
    while (ppsOffset + sizeof(prefix) < parameterBytes &&
           memcmp(parameters + ppsOffset, prefix, sizeof(prefix))) ++ppsOffset;
    PhoenixH264SPS sps;
    if (ppsOffset + sizeof(prefix) >= parameterBytes ||
        !PhoenixH264ReadSPS(parameters + sizeof(prefix), ppsOffset - sizeof(prefix), &sps))
        return reportRejection(reported, SPSFailure, picture, slices, sliceBytes, NULL, 0, NotSupported);

    size_t bytes = parameterBytes + sliceBytes;
    uint8_t *au = malloc(bytes);
    if (!au) return NoMemory;
    memcpy(au, parameters, parameterBytes);
    memcpy(au + parameterBytes, slices, sliceBytes);
    CMVideoDimensions visible = {(int32_t)sps.width, (int32_t)sps.height};
    OSStatus status = PhoenixVADecodeAccessUnit(decoder, au, bytes, visible, pixel);
#if PHX_REFERENCE_DECODE_PROBE
    if (status)
        os_log(OS_LOG_DEFAULT, "Phoenix stream submit status=%{public}d visible=%{public}dx%{public}d surface=%{public}zux%{public}zu format=%{public}#x bytes=%{public}zu",
               (int)status, visible.width, visible.height, CVPixelBufferGetWidth(pixel),
               CVPixelBufferGetHeight(pixel), (unsigned)CVPixelBufferGetPixelFormatType(pixel), bytes);
#endif
    int result = status ? reportRejection(reported, SubmitFailure, picture, slices, sliceBytes, au, bytes, status) : 0;
    free(au);
    return result;
}

int PhoenixAVDDecodeAVCFrame(PhoenixVADecoder *decoder, uint32_t *reported, const PhoenixAVDCommand *command,
                             const PhoenixAVDMappedPool *pools, size_t poolCount) {
    if (!decoder || !reported || !command || !command->picture || !pools) return referenceResult("decode-command", BadParameter);
    if (command->operation != PhoenixAVDDecode || command->function != PhoenixAVDDecodeAVC)
        return referenceResult("decode-command", NotSupported);
    // AMD2 +0x5ef13 selects a mapped bitstream pool and follows the linked
    // NV12 output command to its IOSurface. AppleGVA fills it at 0x7ffa0c58b27f.
    const PhoenixAVDCommand *output = command->next;
    if (!output || output->operation != PhoenixAVDOutput || output->function != PhoenixAVDOutputNV12 ||
        output->next || output->context != command->context) return referenceResult("linked-output", NotSupported);
    const PhoenixAVDDecodeTarget *target = output->picture;
    const PhoenixAVDDecodePicture *picture = command->picture;
    if (!target || !target->surface || !picture->sps || !picture->pps || !picture->slices)
        return referenceResult("decode-descriptors", BadParameter);
    bool reference = PhoenixVADecodeHasReference(decoder) && picture->sliceCount == SliceCount &&
        !memcmp(picture->sps, referenceSPS, sizeof(referenceSPS)) &&
        !memcmp(picture->pps, referencePPS, sizeof(referencePPS));
    if (!reference && !PhoenixVADecodeHasStream(decoder))
        return referenceResult("reference-syntax", NotSupported);
    if (!picture->sliceCount || picture->sliceCount > MaxStreamSlices)
        return referenceResult("slice-count", BadParameter);

    const PhoenixAVDMappedPool *pool = NULL;
    for (size_t i = 0; i < poolCount; ++i)
        if (pools[i].buffers && pools[i].poolID == picture->poolID) { pool = &pools[i]; break; }
    if (!pool || picture->bufferIndex >= pool->count || !pool->buffers[picture->bufferIndex])
        return referenceResult("bitstream-pool", BadParameter);
    const uint8_t *base = pool->buffers[picture->bufferIndex];
    const PhoenixAVDSlice *first = picture->slices[0];
    if (!first || first->offset >= pool->bytesPerBuffer) return referenceResult("slice-range", BadParameter);
    uint32_t end = first->offset;
    for (uint32_t i = 0; i < picture->sliceCount; ++i) {
        const PhoenixAVDSlice *slice = picture->slices[i];
        if (!slice || slice->offset != end || !slice->length || slice->length > pool->bytesPerBuffer - end)
            return referenceResult("slice-range", BadParameter);
        end += slice->length;
    }

    size_t vclBytes = end - first->offset;
    if (vclBytes > PHX_VIDEO_DECODE_STREAM_MAX_INPUT) return referenceResult("access-unit-size", NotSupported);
    CVPixelBufferRef pixel = NULL;
    CVReturn pixelStatus = CVPixelBufferCreateWithIOSurface(NULL, target->surface, NULL, &pixel);
    if (pixelStatus != kCVReturnSuccess) {
        (void)referenceResult("pixel-buffer", pixelStatus);
        return BadParameter;
    }
    OSStatus status;
    if (!reference) {
        int result = submitStream(decoder, reported, picture, base + first->offset, vclBytes, pixel);
        CVPixelBufferRelease(pixel);
        return result;
    }
    status = PhoenixVADecodeFrame(decoder, base + first->offset, vclBytes, pixel);
#if PHX_REFERENCE_DECODE_PROBE
    os_log(OS_LOG_DEFAULT,
           "Phoenix reference IDR diagnostic phase=backend dimensions=%{public}zux%{public}zu format=%{public}#x status=%{public}d",
           CVPixelBufferGetWidth(pixel), CVPixelBufferGetHeight(pixel),
           (unsigned)CVPixelBufferGetPixelFormatType(pixel), (int)status);
#endif
    CVPixelBufferRelease(pixel);
    return status == noErr ? 0 : NotSupported;
}
