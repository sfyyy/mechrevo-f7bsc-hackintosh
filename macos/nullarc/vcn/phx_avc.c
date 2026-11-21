#include "phx_avc_io.h"
#include <VideoToolbox/VTErrors.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static OSStatus packNV12(CVPixelBufferRef image, uint8_t *packed) {
    if (CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetWidth(image) != PhxAVCWidth || CVPixelBufferGetHeight(image) != PhxAVCHeight ||
        CVPixelBufferGetPlaneCount(image) != 2)
        return kVTParameterErr;
    OSStatus status = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    if (status) return status;
    for (size_t plane = 0; plane < 2; ++plane) {
        const size_t rows = PhxAVCHeight >> plane;
        const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
        const uint8_t *src = CVPixelBufferGetBaseAddressOfPlane(image, plane);
        if (!src || pitch < PhxAVCWidth || pitch > SIZE_MAX / rows ||
            CVPixelBufferGetWidthOfPlane(image, plane) != (PhxAVCWidth >> plane) ||
            CVPixelBufferGetHeightOfPlane(image, plane) != rows) {
            status = kVTParameterErr;
            break;
        }
        for (size_t y = 0; y < rows; ++y) {
            memcpy(packed, src + y * pitch, PhxAVCWidth);
            packed += PhxAVCWidth;
        }
    }
    CVReturn unlock = CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    return status ? status : unlock;
}

// Return the first byte of the next prefix and the offset of its NAL header.
// Consumes the full zero run to discard Annex B trailing_zero_8bits.
static size_t nextPrefix(const uint8_t *stream, size_t n, size_t from, size_t *nal) {
    size_t zeros = 0;
    for (size_t i = from; i < n; ++i) {
        if (stream[i] == 1 && zeros >= 2) {
            *nal = i + 1;
            return i - zeros;
        }
        zeros = stream[i] == 0 ? zeros + 1 : 0;
    }
    *nal = n;
    return n;
}

static OSStatus createSample(const uint8_t *stream, size_t n, CMTime pts, CMTime duration,
                             CMSampleBufferRef *sample) {
    size_t nal;
    if (!n || n > PhxAVCMaxBytes || nextPrefix(stream, n, 0, &nal) != 0)
        return kVTVideoEncoderMalfunctionErr;

    // Replacing each three-byte prefix with a four-byte length fits in 2*n.
    uint8_t *payload = malloc(2 * n);
    if (!payload) return kVTAllocationFailedErr;
    const uint8_t *sets[2] = {NULL, NULL};
    size_t lengths[2] = {0, 0}, used = 0;
    bool idr = false;
    CMVideoFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    OSStatus status = kVTVideoEncoderMalfunctionErr;
    for (;;) {
        size_t next;
        size_t end = nextPrefix(stream, n, nal, &next);
        const bool last = end == n;
        while (end > nal && !stream[end - 1]) --end;
        if (end - nal < 2 || (stream[nal] & 0x80)) goto cleanup;
        size_t length = end - nal;
        unsigned type = stream[nal] & 31;
        if (type == 7 || type == 8) {
            if (sets[type - 7] || idr || (type == 8 && !sets[0])) goto cleanup;
            sets[type - 7] = stream + nal;
            lengths[type - 7] = length;
        } else {
            // The fixed firmware packet emits independent IDRs, optionally with SEI/AUD/filler.
            if (type != 5 && type != 6 && type != 9 && type != 12) goto cleanup;
            if (type == 5 && !sets[1]) goto cleanup;
            idr |= type == 5;
            uint32_t be = CFSwapInt32HostToBig((uint32_t)length);
            memcpy(payload + used, &be, sizeof(be));
            memcpy(payload + used + sizeof(be), stream + nal, length);
            used += sizeof(be) + length;
        }
        if (last) break;
        nal = next;
    }
    if (!idr) goto cleanup;
    status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, lengths, 4, &format);
    if (status) goto cleanup;
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    if (dimensions.width != PhxAVCWidth || dimensions.height != PhxAVCHeight) {
        status = kVTVideoEncoderMalfunctionErr;
        goto cleanup;
    }
    status = CMBlockBufferCreateWithMemoryBlock(NULL, payload, used, kCFAllocatorMalloc, NULL,
                                                0, used, 0, &block);
    if (status) goto cleanup;
    payload = NULL; // The block buffer owns the compressed bytes.
    const CMSampleTimingInfo timing = {duration, pts, pts};
    status = CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &used, sample);
cleanup:
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    free(payload);
    return status;
}

OSStatus PhxAVCEncode(io_connect_t connection, CVPixelBufferRef image, CMTime pts,
                      CMTime duration, CMSampleBufferRef *sample) {
    if (!sample) return kVTParameterErr;
    *sample = NULL;
    if (!image || !connection || !CMTIME_IS_NUMERIC(pts) ||
        (CMTIME_IS_VALID(duration) && (!CMTIME_IS_NUMERIC(duration) || duration.value < 0)))
        return kVTParameterErr;
    uint8_t *packed = malloc(PhxAVCInputBytes + PhxAVCMaxBytes);
    if (!packed) return kVTAllocationFailedErr;
    uint8_t *stream = packed + PhxAVCInputBytes;
    size_t bytes = PhxAVCMaxBytes;
    OSStatus status = packNV12(image, packed);
    if (!status) status = PhxAVCSubmit(connection, packed, stream, &bytes);
    if (!status) status = createSample(stream, bytes, pts, duration, sample);
    free(packed);
    return status;
}
