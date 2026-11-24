/* Independently decode the fixed 160x128 VCN encoder probe using the macOS software AVC decoder. */
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { width = 160, height = 128, maxBytes = 28672 };

static void decoded(void *context, void *source, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)source; (void)pts; (void)duration;
    if (status || (flags & kVTDecodeInfo_FrameDropped) || !image ||
        CVPixelBufferGetWidth(image) != width || CVPixelBufferGetHeight(image) != height ||
        CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetPlaneCount(image) != 2 || CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly))
        return;
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    size_t uvPitch = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    bool valid = y && uv && pitch >= width && uvPitch >= width;
    for (unsigned row = 0; row < 2 && valid; ++row) {
        for (unsigned col = 0; col < 2; ++col) {
            const unsigned x = (2 * col + 1) * width / 4, line = (2 * row + 1) * height / 4;
            const uint8_t *chroma = uv + line / 2 * uvPitch + x;
            const int expected = row == col ? 48 : 200;
            valid &= abs(y[line * pitch + x] - expected) <= 20 &&
                     abs(chroma[0] - 128) <= 20 && abs(chroma[1] - 128) <= 20;
        }
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    *(bool *)context = valid;
}

/* The hardware probe produces a single IDR with in-band parameter sets. */
static size_t startCode(const uint8_t *data, size_t size, size_t start, size_t *nal) {
    for (size_t i = start; i + 2 < size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { *nal = i + 3; return i; }
        if (i + 3 < size && !data[i + 2] && data[i + 3] == 1) { *nal = i + 4; return i; }
    }
    *nal = size;
    return size;
}

static bool check(const uint8_t *data, size_t size) {
    size_t nal, begin = startCode(data, size, 0, &nal);
    for (size_t i = 0; i < begin; ++i) if (data[i]) return false;
    const uint8_t *sets[2] = {NULL, NULL};
    size_t sizes[2] = {0, 0}, payloadSize = 0;
    uint8_t payload[2 * maxBytes]; // four-byte AVCC length may replace a three-byte prefix
    bool idr = false;
    while (nal < size) {
        size_t next, end = startCode(data, size, nal, &next);
        while (end > nal && !data[end - 1]) --end;
        const size_t length = end - nal;
        const unsigned type = data[nal] & 31;
        if (length < 2 || data[nal] & 0x80 || !type || type > 23) return false;
        if (type == 7 || type == 8) {
            sets[type - 7] = data + nal;
            sizes[type - 7] = length;
        } else {
            if (type == 5 && (!sets[0] || !sets[1])) return false;
            idr |= type == 5;
            uint32_t lenBE = CFSwapInt32HostToBig((uint32_t)length);
            if (payloadSize + sizeof(lenBE) + length > sizeof(payload)) return false;
            memcpy(payload + payloadSize, &lenBE, sizeof(lenBE));
            memcpy(payload + payloadSize + sizeof(lenBE), data + nal, length);
            payloadSize += sizeof(lenBE) + length;
        }
        nal = next;
    }
    if (!idr) return false;
    CMVideoFormatDescriptionRef format = NULL;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    VTDecompressionSessionRef session = NULL;
    bool matched = false;
    const void *key = kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder;
    const void *value = kCFBooleanFalse;
    CFDictionaryRef spec = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    int pixfmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef px = CFNumberCreate(NULL, kCFNumberIntType, &pixfmt);
    key = kCVPixelBufferPixelFormatTypeKey; value = px;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb = {decoded, &matched};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);
    if (!st) {
        CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(format);
        if (d.width != width || d.height != height) st = paramErr;
    }
    if (!st) st = CMBlockBufferCreateWithMemoryBlock(NULL, payload, payloadSize, kCFAllocatorNull, NULL,
                                                      0, payloadSize, 0, &block);
    if (!st) st = CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &payloadSize, &sample);
    if (!st) st = VTDecompressionSessionCreate(NULL, format, spec, attrs, &cb, &session);
    if (!st) st = VTDecompressionSessionDecodeFrame(session, sample, 0, NULL, NULL);
    if (!st) st = VTDecompressionSessionFinishDelayedFrames(session);
    if (!st) st = VTDecompressionSessionWaitForAsynchronousFrames(session);
    if (session) { VTDecompressionSessionInvalidate(session); CFRelease(session); }
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    if (format) CFRelease(format);
    CFRelease(attrs); CFRelease(px); CFRelease(spec);
    if (st) fprintf(stderr, "AVC decode returned %d\n", (int)st);
    return !st && matched;
}

int main(int argc, char **argv) {
    if (argc != 2) { fputs("usage: vcn_frame_check frame.h264\n", stderr); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror(argv[1]); return 1; }
    uint8_t data[maxBytes + 1];
    size_t size = fread(data, 1, sizeof(data), file);
    bool ok = !ferror(file) && size && size <= maxBytes;
    fclose(file);
    if (!ok || !check(data, size)) { fputs("invalid AVC frame or unexpected NV12 pattern\n", stderr); return 1; }
    puts("AVC software decode verified 160x128, four luma quadrants and neutral chroma");
    return 0;
}
