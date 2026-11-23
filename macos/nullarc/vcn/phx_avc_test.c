/* Link with phx_avc.c only. The transport verifies every input byte before returning a known access unit. */
#include "phx_avc_io.h"
#include <VideoToolbox/VideoToolbox.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

enum { fixtureBytes = 180, yStride = 216, uvStride = 192 };
static const io_connect_t testConnection = 123;
static uint8_t fixture[fixtureBytes], reply[fixtureBytes + 8], packed[PhxAVCInputBytes];
static size_t replyBytes;
static unsigned submissions;
static CMVideoDimensions expected = {PhxAVCWidth, PhxAVCHeight};
static OSStatus submitStatus;

OSStatus PhxAVCSubmit(io_connect_t connection, uint32_t width, uint32_t height,
                      const uint8_t *nv12, uint8_t *annexB, size_t *bytes) {
    ++submissions;
    CHECK(width == (unsigned)expected.width && height == (unsigned)expected.height);
    CHECK(connection == testConnection && *bytes == phxAVCOutputCapacity(width, height));
    if (width == PhxAVCWidth && height == PhxAVCHeight)
        CHECK(memcmp(nv12, packed, sizeof(packed)) == 0);
    if (submitStatus) return submitStatus;
    if (replyBytes <= sizeof(reply)) memcpy(annexB, reply, replyBytes);
    *bytes = replyBytes;
    return noErr;
}

static CVPixelBufferRef makeFrame(OSType format) {
    static uint8_t y[yStride * PhxAVCHeight], uv[uvStride * PhxAVCHeight / 2];
    void *planes[] = {y, uv};
    size_t widths[] = {PhxAVCWidth, PhxAVCWidth/2};
    size_t heights[] = {PhxAVCHeight, PhxAVCHeight/2};
    size_t strides[] = {yStride, uvStride};
    for (size_t i = 0; i < sizeof(packed); ++i)
        packed[i] = (uint8_t)(17 * i + 13 * (i / PhxAVCWidth) + 79);
    memset(y, 0xcd, sizeof(y));
    memset(uv, 0x29, sizeof(uv));
    for (size_t row = 0; row < heights[0]; ++row)
        memcpy(y + row * yStride, packed + row * PhxAVCWidth, PhxAVCWidth);
    for (size_t row = 0; row < heights[1]; ++row)
        memcpy(uv + row * uvStride, packed + (row + PhxAVCHeight) * PhxAVCWidth, PhxAVCWidth);
    CVPixelBufferRef image = NULL;
    CHECK(CVPixelBufferCreateWithPlanarBytes(NULL, PhxAVCWidth, PhxAVCHeight,
          format, NULL, 0, 2, planes, widths, heights, strides,
          NULL, NULL, NULL, &image) == kCVReturnSuccess);
    if (format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        for (size_t i = 0; i < sizeof(packed); ++i) {
            const double span = i < PhxAVCWidth * PhxAVCHeight ? 219.0 : 224.0;
            packed[i] = (uint8_t)(16.5 + packed[i] * span / 255.0);
        }
    }
    return image;
}

static void expectInvalid(io_connect_t connection, CVPixelBufferRef image, CMTime pts, CMTime duration) {
    CMSampleBufferRef sample = (CMSampleBufferRef)(uintptr_t)1;
    CHECK(PhxAVCEncode(connection, image, pts, duration, &sample) != noErr && sample == NULL);
}

static void checkInputs(CVPixelBufferRef image, CMTime pts, CMTime duration) {
    CHECK(PhxAVCEncode(testConnection, image, pts, duration, NULL) != noErr);
    expectInvalid(0, image, pts, duration);
    expectInvalid(testConnection, NULL, pts, duration);
    expectInvalid(testConnection, image, kCMTimeIndefinite, duration);
    expectInvalid(testConnection, image, pts, CMTimeMake(-1, 30));
    const OSType formats[] = {kCVPixelFormatType_420YpCbCr8Planar,
                              kCVPixelFormatType_32BGRA, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange};
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i) {
        CVPixelBufferRef bad = NULL;
        size_t width = i == 2 ? 96 : PhxAVCWidth;
        CHECK(CVPixelBufferCreate(NULL, width, PhxAVCHeight, formats[i], NULL, &bad) == kCVReturnSuccess);
        expectInvalid(testConnection, bad, pts, duration);
        CFRelease(bad);
    }
    CHECK(submissions == 0);
}

static void checkReplies(CVPixelBufferRef image, CMTime pts, CMTime duration) {
    CMSampleBufferRef sample = NULL;
    submitStatus = kVTVideoEncoderNotAvailableNowErr;
    CHECK(PhxAVCEncode(testConnection, image, pts, duration, &sample) == submitStatus && !sample);
    submitStatus = noErr;
    memcpy(reply, fixture, fixtureBytes);
    const size_t invalidSizes[] = {0, 4, 31, PhxAVCMaxBytes + 1u, SIZE_MAX};
    for (size_t i = 0; i < sizeof(invalidSizes)/sizeof(invalidSizes[0]); ++i) {
        replyBytes = invalidSizes[i];
        expectInvalid(testConnection, image, pts, duration);
    }
    replyBytes = fixtureBytes;
    reply[4] |= 0x80;  /* forbidden_zero_bit */
    expectInvalid(testConnection, image, pts, duration);
    memcpy(reply, fixture, fixtureBytes);
    reply[27] = 6;     /* remove PPS before the first IDR */
    expectInvalid(testConnection, image, pts, duration);
    memcpy(reply, fixture, fixtureBytes);
    reply[75] = reply[131] = 0x21; /* non-IDR slices */
    expectInvalid(testConnection, image, pts, duration);
    memcpy(reply, fixture, fixtureBytes);
    memcpy(reply + fixtureBytes, "\0\0\1", 3);
    replyBytes = fixtureBytes + 3;
    expectInvalid(testConnection, image, pts, duration);
}

static void checkSample(CMSampleBufferRef sample, CMTime pts, CMTime duration) {
    CHECK(sample && CMSampleBufferIsValid(sample) && CMSampleBufferDataIsReady(sample));
    CHECK(CMSampleBufferGetNumSamples(sample) == 1);
    CHECK(CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample), pts) == 0);
    CHECK(CMTimeCompare(CMSampleBufferGetDuration(sample), duration) == 0);
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    CHECK(format && CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_H264);
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format);
    CHECK(dim.width == PhxAVCWidth && dim.height == PhxAVCHeight);
    /* Payload offsets in the checked-in vcn_quadrants.h264 fixture. */
    const size_t offsets[] = {4, 27, 35, 50, 75, 131};
    const size_t lengths[] = {19, 4, 11, 21, 52, 49};
    for (size_t i = 0; i < 2; ++i) {
        const uint8_t *set = NULL;
        size_t size, count;
        int naluBytes;
        CHECK(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &size,
                                                                  &count, &naluBytes) == noErr);
        CHECK(count == 2 && naluBytes == 4 && size == lengths[i]);
        CHECK(memcmp(set, fixture + offsets[i], size) == 0);
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    CHECK(block && CMSampleBufferGetSampleSize(sample, 0) == CMBlockBufferGetDataLength(block));
    size_t at = 0;
    for (size_t i = 2; i < 6; ++i) {
        uint32_t count;
        uint8_t bytes[fixtureBytes];
        CHECK(CMBlockBufferCopyDataBytes(block, at, sizeof(count), &count) == noErr);
        CHECK(CFSwapInt32BigToHost(count) == lengths[i]);
        at += sizeof(count);
        CHECK(CMBlockBufferCopyDataBytes(block, at, lengths[i], bytes) == noErr);
        CHECK(memcmp(bytes, fixture + offsets[i], lengths[i]) == 0);
        at += lengths[i];
    }
    CHECK(CMBlockBufferGetDataLength(block) == at);
}

static void decoded(void *ref, void *source, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)source; (void)pts; (void)duration;
    CHECK(status == noErr && !(flags & kVTDecodeInfo_FrameDropped) && image);
    CHECK(CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CHECK(CVPixelBufferGetWidth(image) == PhxAVCWidth && CVPixelBufferGetHeight(image) == PhxAVCHeight);
    CHECK(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess);
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned col = 0; col < 2; ++col) {
            int pixel = y[((2*row + 1)*PhxAVCHeight/4)*pitch + (2*col + 1)*PhxAVCWidth/4];
            CHECK(abs(pixel - (row == col ? 48 : 200)) <= 20);
        }
    CHECK(CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess);
    ++*(unsigned *)ref;
}

static void checkSoftwareDecode(CMSampleBufferRef sample) {
    int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *key = kCVPixelBufferPixelFormatTypeKey, *value = number;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    key = kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder; value = kCFBooleanFalse;
    CFDictionaryRef spec = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    unsigned frames = 0;
    VTDecompressionOutputCallbackRecord cb = {decoded, &frames};
    VTDecompressionSessionRef session = NULL;
    OSStatus status = VTDecompressionSessionCreate(NULL, CMSampleBufferGetFormatDescription(sample), spec,
                                                   attrs, &cb, &session);
    if (status) fprintf(stderr, "VTDecompressionSessionCreate returned %d\n", (int)status);
    CHECK(status == noErr);
    CHECK(VTDecompressionSessionDecodeFrame(session, sample, 0, NULL, NULL) == noErr);
    CHECK(VTDecompressionSessionFinishDelayedFrames(session) == noErr);
    CHECK(VTDecompressionSessionWaitForAsynchronousFrames(session) == noErr && frames == 1);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session); CFRelease(attrs); CFRelease(spec); CFRelease(number);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--decode") != 0)) {
        fputs("usage: phx_avc_test vcn_quadrants.h264 [--decode]\n", stderr);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    CHECK(file && fread(fixture, 1, fixtureBytes, file) == fixtureBytes);
    CHECK(fgetc(file) == EOF && !ferror(file) && fclose(file) == 0);
    CVPixelBufferRef image = makeFrame(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CMTime pts = CMTimeMake(17, 30), duration = CMTimeMake(1, 30);
    checkInputs(image, pts, duration);
    checkReplies(image, pts, duration);
    /* Also verify a three-byte prefix and trailing_zero_8bits. */
    memcpy(reply, fixture + 1, fixtureBytes - 1);
    memset(reply + fixtureBytes - 1, 0, 3);
    replyBytes = fixtureBytes + 2;
    CFDataRef accessUnit = NULL;
    CHECK(PhxAVCCopyAnnexB(testConnection, image, &accessUnit) == noErr && accessUnit);
    CHECK((size_t)CFDataGetLength(accessUnit) == replyBytes);
    CHECK(memcmp(CFDataGetBytePtr(accessUnit), reply, replyBytes) == 0);
    CFRelease(accessUnit);
    CMSampleBufferRef first = NULL, second = NULL;
    CHECK(PhxAVCEncode(testConnection, image, pts, duration, &first) == noErr);
    CMTime next = CMTimeAdd(pts, duration);
    CHECK(PhxAVCEncode(testConnection, image, next, duration, &second) == noErr);
    checkSample(second, next, duration);
    CFRelease(second); CFRelease(image);
    memset(reply, 0, sizeof(reply));
    checkSample(first, pts, duration);
    if (argc == 3) checkSoftwareDecode(first);
    CFRelease(first);
    image = makeFrame(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
    memcpy(reply, fixture, fixtureBytes);
    replyBytes = fixtureBytes;
    CHECK(PhxAVCEncode(testConnection, image, pts, duration, &second) == noErr);
    checkSample(second, pts, duration);
    CFRelease(second);
    CFRelease(image);
    expected = (CMVideoDimensions){960, 540};
    CHECK(CVPixelBufferCreate(NULL, expected.width, expected.height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              NULL, &image) == kCVReturnSuccess);
    // Reject 160x128 parameter sets for the larger requested frame.
    CHECK(PhxAVCEncode(testConnection, image, pts, duration, &second) == kVTVideoEncoderMalfunctionErr && !second);
    CFRelease(image);
    puts("Phoenix AVC adapter host tests passed (420v and 420f)");
    return 0;
}
