// Exercise the real VideoToolbox provider with a replayed VCN response.
#include "phx_avc_io.h"
#include "phx_vt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { frameCount = 8 };
static uint8_t annexB[PhxAVCMaxBytes];
static size_t annexBBytes;
static bool available;
static unsigned opens, closes, inputs, outputs, bad, callbackErrors;
static const io_connect_t testConnection = 93;

OSStatus PhxAVCOpen(io_connect_t *connection) {
    *connection = available ? testConnection : IO_OBJECT_NULL;
    opens += available;
    return available ? noErr : kVTVideoEncoderNotAvailableNowErr;
}

kern_return_t IOServiceClose(io_connect_t connection) {
    if (connection != testConnection) return kIOReturnBadArgument;
    ++closes;
    return KERN_SUCCESS;
}

OSStatus PhxAVCSubmit(io_connect_t connection, const uint8_t *nv12, uint8_t *out, size_t *size) {
    if (nv12[0] == 25 + frameCount) return kVTVideoEncoderMalfunctionErr;
    if (connection != testConnection || *size < annexBBytes) return kVTParameterErr;
    if (nv12[0] != 25 + inputs) ++bad;
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
        CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample), CMTimeMake(outputs, 30)) ||
        CMTimeCompare(CMSampleBufferGetDuration(sample), CMTimeMake(1, 30)) ||
        !CMSampleBufferGetTotalSampleSize(sample)) ++bad;
    ++outputs;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file);
    annexBBytes = fread(annexB, 1, sizeof(annexB), file);
    CHECK(annexBBytes && feof(file) && !ferror(file));
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
    CHECK(VTCompressionSessionCreate(NULL, 320, 192, kCMVideoCodecType_H264, spec, NULL, NULL,
                                     receive, NULL, &session) != noErr && !session);
    CHECK(VTCompressionSessionCreate(NULL, PhxAVCWidth, PhxAVCHeight, kCMVideoCodecType_H264,
                                     spec, NULL, NULL, receive, NULL, &session) == noErr);
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
        *(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 0) = (uint8_t)(25 + i);
        CHECK(CVPixelBufferUnlockBaseAddress(pixel, 0) == kCVReturnSuccess);
        if (i == frameCount) CHECK(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
        OSStatus encoded = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(i, 30),
            CMTimeMake(1, 30), NULL, (void *)(uintptr_t)(i + 1), NULL);
        CFRelease(pixel);
        if (i == frameCount) {
            CHECK(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
            CHECK(encoded == kVTVideoEncoderMalfunctionErr || (encoded == noErr && callbackErrors == 1));
        }
        else CHECK(encoded == noErr);
    }
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CHECK(inputs == frameCount && outputs == frameCount && !bad && opens == closes);
    puts("PASS VideoToolbox AVC provider, 8 frames, mock I/O");
    return 0;
}
