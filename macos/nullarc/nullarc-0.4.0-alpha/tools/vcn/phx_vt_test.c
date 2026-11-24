// Exercise the real VideoToolbox provider with a replayed VCN response.
#include "phx_avc_io.h"
#include "phx_vt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { frameCount = 8, frameRate = 25 };
static uint8_t *annexB;
static size_t annexBBytes;
static bool available;
static uint32_t requestedWidth = PhxAVCWidth, requestedHeight = PhxAVCHeight;
static bool fullRange;
static unsigned opens, closes, inputs, outputs, bad, callbackErrors;
static const io_connect_t testConnection = 93;

OSStatus PhxAVCOpen(io_connect_t *connection) {
    *connection = available ? testConnection : IO_OBJECT_NULL;
    opens += available;
    return available ? noErr : kVTVideoEncoderNotAvailableNowErr;
}

OSStatus PhxAVCOpenForDimensions(CMVideoDimensions dimensions, io_connect_t *connection) {
    if ((uint32_t)dimensions.width != requestedWidth || (uint32_t)dimensions.height != requestedHeight) {
        *connection = IO_OBJECT_NULL;
        return kVTParameterErr;
    }
    return PhxAVCOpen(connection);
}

kern_return_t IOServiceClose(io_connect_t connection) {
    if (connection != testConnection) return kIOReturnBadArgument;
    ++closes;
    return KERN_SUCCESS;
}

static uint8_t expectedLuma(unsigned i) {
    unsigned luma = 24 + 8 * i;
    return fullRange ? (uint8_t)(16.5 + luma * 219.0 / 255.0) : (uint8_t)luma;
}

OSStatus PhxAVCSubmit(io_connect_t connection, uint32_t width, uint32_t height,
                      const uint8_t *nv12, uint8_t *out, size_t *size) {
    if (width != requestedWidth || height != requestedHeight || *size != phxAVCOutputCapacity(width, height))
        return kVTParameterErr;
    if (nv12[0] == expectedLuma(frameCount)) return kVTVideoEncoderMalfunctionErr;
    if (connection != testConnection || *size < annexBBytes) return kVTParameterErr;
    if (nv12[0] != expectedLuma(inputs)) ++bad;
    memcpy(out, annexB, annexBBytes);
    *size = annexBBytes;
    ++inputs;
    return noErr;
}

static void receive(void *context, void *source, OSStatus status, VTEncodeInfoFlags flags,
                    CMSampleBufferRef sample) {
    (void)context;
    if ((uintptr_t)source == frameCount + 1 && status == kVTVideoEncoderMalfunctionErr && !sample) {
        ++callbackErrors;
        return;
    }
    if (status || !sample || (flags & kVTEncodeInfo_FrameDropped) ||
        (uintptr_t)source != outputs + 1 ||
        CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample), CMTimeMake(outputs, frameRate)) ||
        CMTimeCompare(CMSampleBufferGetDuration(sample), CMTimeMake(1, frameRate)) ||
        !CMSampleBufferGetTotalSampleSize(sample)) ++bad;
    if (sample) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        CGSize visible = CMVideoFormatDescriptionGetPresentationDimensions(format, false, true);
        if (visible.width != requestedWidth || visible.height != requestedHeight) ++bad;
    }
    ++outputs;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

int main(int argc, char **argv) {
    // An optional fixture from another encoder may exercise a conference-size 420f session.
    CHECK(argc == 2 || argc == 4);
    if (argc == 4) {
        char *end;
        unsigned long width = strtoul(argv[2], &end, 10);
        CHECK(!*end && width <= UINT32_MAX);
        unsigned long height = strtoul(argv[3], &end, 10);
        CHECK(!*end && height <= UINT32_MAX && phxnVcnSizeSupported((uint32_t)width, (uint32_t)height));
        requestedWidth = (uint32_t)width;
        requestedHeight = (uint32_t)height;
        fullRange = true;
    }
    FILE *file = fopen(argv[1], "rb");
    CHECK(file);
    size_t capacity = phxAVCOutputCapacity(requestedWidth, requestedHeight);
    annexB = malloc(capacity + 1);
    CHECK(annexB);
    annexBBytes = fread(annexB, 1, capacity + 1, file);
    CHECK(annexBBytes && annexBBytes <= capacity && feof(file) && !ferror(file));
    fclose(file);
    CHECK(PhxRegisterAVCEncoder() == kVTVideoEncoderNotAvailableNowErr);
    available = true;
    CHECK(PhxRegisterAVCEncoder() == noErr);
    CHECK(PhxRegisterAVCEncoder() == noErr && opens == 1 && closes == 1);
    const void *keys[] = {kVTVideoEncoderSpecification_EncoderID,
                          kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
    const void *values[] = {PhxAVCEncoderID, kCFBooleanTrue};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CHECK(spec);
    VTCompressionSessionRef session = NULL;
    CHECK(VTCompressionSessionCreate(NULL, 321, 192, kCMVideoCodecType_H264, spec, NULL, NULL,
                                     receive, NULL, &session) != noErr && !session);
    int32_t fmt = fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    const void *key = kCVPixelBufferPixelFormatTypeKey, *value = number;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    CHECK(VTCompressionSessionCreate(NULL, requestedWidth, requestedHeight, kCMVideoCodecType_H264,
                                     spec, attrs, NULL, receive, NULL, &session) == noErr);
    CFRelease(attrs);
    CFRelease(number);
    CFRelease(spec);
    CFTypeRef hardware = NULL, encoderID = NULL;
    CHECK(VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                NULL, &hardware) == noErr && hardware == kCFBooleanTrue);
    CFRelease(hardware);
    CHECK(VTSessionCopyProperty(session, kVTCompressionPropertyKey_EncoderID, NULL, &encoderID) == noErr &&
          encoderID && CFEqual(encoderID, PhxAVCEncoderID));
    CFRelease(encoderID);
    CHECK(VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) == noErr);
    CHECK(VTCompressionSessionPrepareToEncodeFrames(session) == noErr);
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    CHECK(pool);
    for (unsigned i = 0; i <= frameCount; ++i) {
        CVPixelBufferRef pixel = NULL;
        CHECK(CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixel) == kCVReturnSuccess);
        CHECK(CVPixelBufferLockBaseAddress(pixel, 0) == kCVReturnSuccess);
        CHECK(CVPixelBufferGetWidth(pixel) == requestedWidth && CVPixelBufferGetHeight(pixel) == requestedHeight);
        CHECK(CVPixelBufferGetPixelFormatType(pixel) == (OSType)fmt);
        *(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 0) = (uint8_t)(24 + 8 * i);
        CHECK(CVPixelBufferUnlockBaseAddress(pixel, 0) == kCVReturnSuccess);
        if (i == frameCount) CHECK(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
        OSStatus encoded = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(i, frameRate),
            CMTimeMake(1, frameRate), NULL, (void *)(uintptr_t)(i + 1), NULL);
        CFRelease(pixel);
        if (i == frameCount) {
            CHECK(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
            CHECK(encoded == kVTVideoEncoderMalfunctionErr || (encoded == noErr && callbackErrors == 1));
        }
        else CHECK(encoded == noErr);
    }
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    free(annexB);
    CHECK(inputs == frameCount && outputs == frameCount && !bad && opens == closes);
    printf("PASS VideoToolbox AVC provider, %ux%u, 8 frames, mock I/O\n", requestedWidth, requestedHeight);
    return 0;
}
