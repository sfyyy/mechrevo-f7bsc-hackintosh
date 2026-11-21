/* Tests the public VideoToolbox path used by applications. Defaults to requiring hardware. */
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PHX_VT_ENCODER
#include "phx_avc.h"
#include "phx_vt.h"
#endif

enum { frameCount = 8, frameRate = 30 };
static unsigned width = 320, height = 192;
static const OSType pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

typedef struct {
    pthread_mutex_t lock;
    CMSampleBufferRef samples[frameCount];
    unsigned count;
    bool failed;
} Encoded;

typedef struct {
    pthread_mutex_t lock;
    unsigned seen;
    bool failed;
} Decoded;

static int frameIndex(CMTime pts) {
    for (int i = 0; i < frameCount; ++i) {
        if (CMTIME_IS_NUMERIC(pts) && CMTimeCompare(pts, CMTimeMake(i, frameRate)) == 0)
            return i;
    }
    return -1;
}

static uint8_t luma(int frame, bool right) {
    const bool bright = ((frame & 1) != 0) == right;
    return bright ? 200 : 48;
}

static uint8_t chroma(int frame, bool bottom, bool cr) {
    // Luma carries frame bit 0. Cb and Cr carry bits 1 and 2 respectively;
    // reversing both across the horizontal boundary also checks plane layout.
    const int mask = cr ? 4 : 2;
    return (((frame & mask) != 0) ^ bottom ^ cr) ? 176 : 80;
}

static void encodedFrame(void *ref, void *source, OSStatus status, VTEncodeInfoFlags flags,
                         CMSampleBufferRef sample) {
    (void)source;
    Encoded *out = ref;
    pthread_mutex_lock(&out->lock);
    if (status != noErr || (flags & kVTEncodeInfo_FrameDropped) || !sample ||
        !CMSampleBufferDataIsReady(sample) || !CMSampleBufferGetDataBuffer(sample) ||
        CMSampleBufferGetTotalSampleSize(sample) == 0 || out->count >= frameCount) {
        fprintf(stderr, "  encode callback: status %d flags 0x%x\n", (int)status, (unsigned)flags);
        out->failed = true;
    } else {
        // Preserve callback order. VideoToolbox delivers compressed samples in decode order.
        out->samples[out->count++] = (CMSampleBufferRef)CFRetain(sample);
    }
    pthread_mutex_unlock(&out->lock);
}

static bool validNV12(CVImageBufferRef image) {
    return image && CVPixelBufferGetWidth(image) == width && CVPixelBufferGetHeight(image) == height &&
           CVPixelBufferGetPixelFormatType(image) == pixelFormat && CVPixelBufferGetPlaneCount(image) == 2 &&
           CVPixelBufferGetIOSurface(image);
}

static void decodedFrame(void *ref, void *source, OSStatus status, VTDecodeInfoFlags flags,
                         CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)source;
    (void)duration;
    Decoded *out = ref;
    const int index = frameIndex(pts);
    bool valid = status == noErr && !(flags & kVTDecodeInfo_FrameDropped) && index >= 0 && validNV12(image);
    if (valid) {
        valid = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
        if (valid) {
            const uint8_t *plane = CVPixelBufferGetBaseAddressOfPlane(image, 0);
            const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
            const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(image, 1);
            const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
            valid = plane && uv && stride >= width && uvStride >= width;
            if (valid) {
                const uint8_t *row = plane + (height / 2) * stride;
                valid = abs((int)row[width / 4] - luma(index, false)) <= 20 &&
                        abs((int)row[width * 3 / 4] - luma(index, true)) <= 20;
                for (unsigned half = 0; half < 2; ++half) {
                    const uint8_t *pair = uv + ((2 * half + 1) * height / 8) * uvStride + width / 2;
                    valid = valid && abs(pair[0] - chroma(index, half, false)) <= 20 &&
                                     abs(pair[1] - chroma(index, half, true)) <= 20;
                }
            }
            CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        }
    }
    pthread_mutex_lock(&out->lock);
    if (!valid || (out->seen & (1u << index))) {
        fprintf(stderr, "  decode callback: status %d flags 0x%x, frame %d or NV12 content invalid\n",
                (int)status, (unsigned)flags, index);
        out->failed = true;
    } else {
        out->seen |= 1u << index;
    }
    pthread_mutex_unlock(&out->lock);
}

static CFDictionaryRef surfaceAttributes(void) {
    int fmt = (int)pixelFormat;
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {number, surface};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(surface);
    CFRelease(number);
    return attrs;
}

static CFDictionaryRef sessionSpecification(bool encode, bool hardware, CFNumberRef gpu, CFStringRef encoderID) {
    CFStringRef key, gpuKey;
    if (encode) {
        key = hardware ? kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder
                       : kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder;
        gpuKey = kVTVideoEncoderSpecification_RequiredEncoderGPURegistryID;
    } else {
        key = hardware ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                       : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder;
        gpuKey = kVTVideoDecoderSpecification_RequiredDecoderGPURegistryID;
    }
    const void *keys[3] = {key};
    const void *values[3] = {hardware ? kCFBooleanTrue : kCFBooleanFalse};
    CFIndex count = 1;
    if (gpu) { keys[count] = gpuKey; values[count++] = gpu; }
    if (encoderID) { keys[count] = kVTVideoEncoderSpecification_EncoderID; values[count++] = encoderID; }
    return CFDictionaryCreate(NULL, keys, values, count, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
}

static bool checkAcceleration(VTSessionRef session, CFStringRef key, bool required) {
    CFTypeRef value = NULL;
    OSStatus status = VTSessionCopyProperty(session, key, NULL, &value);
    if (!required && status == kVTPropertyNotSupportedErr) {
        if (value)
            CFRelease(value);
        return true;
    }
    if (status != noErr || !value || CFGetTypeID(value) != CFBooleanGetTypeID()) {
        fprintf(stderr, "  hardware property query failed (status %d)\n", (int)status);
        if (value)
            CFRelease(value);
        return false;
    }
    const bool accelerated = CFBooleanGetValue((CFBooleanRef)value);
    if (value)
        CFRelease(value);
    if (accelerated != required) {
        fprintf(stderr, "  hardware in use = %d (query status %d), expected %d\n", accelerated, (int)status, required);
        return false;
    }
    return true;
}

static OSStatus encodeFrame(VTCompressionSessionRef session, int index) {
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    CVPixelBufferRef pixel = NULL;
    CVReturn status = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixel) : kCVReturnInvalidArgument;
    if (status != kCVReturnSuccess)
        return status;
    if (!validNV12(pixel)) {
        CFRelease(pixel);
        return kCVReturnInvalidPixelFormat;
    }
    status = CVPixelBufferLockBaseAddress(pixel, 0);
    if (status == kCVReturnSuccess) {
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
        uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pixel, 1);
        const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
        const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
        for (size_t row = 0; row < height; ++row) {
            memset(y + row * yStride, luma(index, false), width / 2);
            memset(y + row * yStride + width / 2, luma(index, true), width / 2);
        }
        for (unsigned row = 0; row < height / 2; ++row) {
            uint8_t *line = uv + row * uvStride;
            for (unsigned x = 0; x < width; x += 2) {
                line[x] = chroma(index, row >= height / 4, false);
                line[x + 1] = chroma(index, row >= height / 4, true);
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        status = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(index, frameRate),
                                                  CMTimeMake(1, frameRate), NULL, NULL, NULL);
    }
    CFRelease(pixel);
    return status;
}

static bool encode(CMVideoCodecType codec, bool hardware, CFNumberRef gpu, CFStringRef encoderID,
                   CFDictionaryRef attrs, Encoded *out) {
    CFDictionaryRef spec = sessionSpecification(true, hardware, gpu, encoderID);
    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(NULL, width, height, codec, spec, attrs, NULL,
                                                 encodedFrame, out, &session);
    CFRelease(spec);
    if (status != noErr) {
        fprintf(stderr, "  VTCompressionSessionCreate: %d\n", (int)status);
        return false;
    }
    status = VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if (status == noErr)
        status = VTCompressionSessionPrepareToEncodeFrames(session);
    for (int i = 0; status == noErr && i < frameCount; ++i)
        status = encodeFrame(session, i);
    if (status == noErr)
        status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    const bool accelerated = status == noErr &&
        checkAcceleration(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, hardware);
    if (status == noErr && encoderID) {
        CFTypeRef selected = NULL;
        status = VTSessionCopyProperty(session, kVTCompressionPropertyKey_EncoderID, NULL, &selected);
        if (!status && (!selected || !CFEqual(selected, encoderID))) status = kVTCouldNotFindVideoEncoderErr;
        if (selected) CFRelease(selected);
    }
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    if (status != noErr)
        fprintf(stderr, "  encode frames: %d\n", (int)status);
    unsigned seen = 0;
    for (unsigned i = 0; i < out->count; ++i) {
        CMSampleBufferRef sample = out->samples[i];
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        const int index = frameIndex(CMSampleBufferGetPresentationTimeStamp(sample));
        if (!format || CMFormatDescriptionGetMediaSubType(format) != codec || index < 0 || (seen & (1u << index)))
            return false;
        seen |= 1u << index;
    }
    return status == noErr && accelerated && !out->failed && out->count == frameCount;
}

static bool decode(const Encoded *encoded, bool hardware, CFNumberRef gpu, CFDictionaryRef attrs) {
    CFDictionaryRef spec = sessionSpecification(false, hardware, gpu, NULL);
    Decoded out = {.lock = PTHREAD_MUTEX_INITIALIZER};
    VTDecompressionOutputCallbackRecord callback = {decodedFrame, &out};
    VTDecompressionSessionRef session = NULL;
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(encoded->samples[0]);
    OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, attrs, &callback, &session);
    CFRelease(spec);
    if (status != noErr) {
        fprintf(stderr, "  VTDecompressionSessionCreate: %d\n", (int)status);
        pthread_mutex_destroy(&out.lock);
        return false;
    }
    for (unsigned i = 0; status == noErr && i < encoded->count; ++i) {
        status = VTDecompressionSessionDecodeFrame(session, encoded->samples[i],
                                                   kVTDecodeFrame_EnableAsynchronousDecompression, NULL, NULL);
    }
    if (status == noErr)
        status = VTDecompressionSessionFinishDelayedFrames(session);
    if (status == noErr)
        status = VTDecompressionSessionWaitForAsynchronousFrames(session);
    const bool accelerated = status == noErr &&
        checkAcceleration(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, hardware);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    pthread_mutex_destroy(&out.lock);
    if (status != noErr)
        fprintf(stderr, "  decode frames: %d\n", (int)status);
    return status == noErr && accelerated && !out.failed && out.seen == (1u << frameCount) - 1u;
}

static void releaseSamples(Encoded *out) {
    for (unsigned i = 0; i < out->count; ++i) {
        CFRelease(out->samples[i]);
        out->samples[i] = NULL;
    }
    out->count = 0;
    out->failed = false;
}

static bool testCodec(const char *name, CMVideoCodecType codec, bool hardware, CFNumberRef gpu, CFDictionaryRef attrs) {
    Encoded encoded = {.lock = PTHREAD_MUTEX_INITIALIZER};
    printf("%s hardware decode advertised: %s\n", name, VTIsHardwareDecodeSupported(codec) ? "yes" : "no");
    printf("%s %s encode\n", name, hardware ? "hardware" : "software");
    const bool encodeOK = encode(codec, hardware, gpu, NULL, attrs, &encoded);
    printf("  %s (%u/%d compressed frames)\n", encodeOK ? "PASS" : "FAIL", encoded.count, frameCount);
    bool haveInput = encodeOK;
    if (!haveInput && hardware) {
        releaseSamples(&encoded);
        printf("%s software encode for the hardware decoder's input\n", name);
        haveInput = encode(codec, false, NULL, NULL, attrs, &encoded);
    }
    printf("%s %s decode\n", name, hardware ? "hardware" : "software");
    const bool decodeOK = haveInput && decode(&encoded, hardware, gpu, attrs);
    printf("  %s\n", decodeOK ? "PASS" : (haveInput ? "FAIL" : "BLOCKED (no compressed sample)"));
    releaseSamples(&encoded);
    pthread_mutex_destroy(&encoded.lock);
    return encodeOK && decodeOK;
}

int main(int argc, char **argv) {
    bool hardware = true;
    CFNumberRef gpu = NULL;
#ifdef PHX_VT_ENCODER
    if (argc == 2 && strcmp(argv[1], "--phx-avc") == 0) {
        OSStatus status = PhxRegisterAVCEncoder();
        if (status) {
            fprintf(stderr, "Phoenix encoder registration: %d\n", (int)status);
            return 1;
        }
        width = PhxAVCWidth;
        height = PhxAVCHeight;
        CFDictionaryRef attrs = surfaceAttributes();
        Encoded encoded = {.lock = PTHREAD_MUTEX_INITIALIZER};
        bool ok = encode(kCMVideoCodecType_H264, true, NULL, PhxAVCEncoderID, attrs, &encoded);
        printf("Phoenix H.264 VT hardware encode: %s (%u/%d)\n", ok ? "PASS" : "FAIL", encoded.count, frameCount);
        if (ok) {
            ok = decode(&encoded, false, NULL, attrs);
            printf("Independent software decode and Y/UV check: %s\n", ok ? "PASS" : "FAIL");
        }
        releaseSamples(&encoded);
        pthread_mutex_destroy(&encoded.lock);
        CFRelease(attrs);
        return ok ? 0 : 1;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "--software") == 0) {
        hardware = false;
    } else if (argc == 3 && strcmp(argv[1], "--gpu-id") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long long value = strtoull(argv[2], &end, 0);
        if (!errno && end != argv[2] && !*end && *argv[2] >= '0' && *argv[2] <= '9' &&
            value > 0 && value <= INT64_MAX) {
            int64_t id = (int64_t)value;
            gpu = CFNumberCreate(NULL, kCFNumberSInt64Type, &id);
        }
        if (!gpu) {
            fputs("invalid GPU registry ID\n", stderr);
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--software | --gpu-id ID]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    CFDictionaryRef attrs = surfaceAttributes();
    const bool avc = testCodec("H.264", kCMVideoCodecType_H264, hardware, gpu, attrs);
    const bool hevc = testCodec("HEVC", kCMVideoCodecType_HEVC, hardware, gpu, attrs);
    CFRelease(attrs);
    if (gpu)
        CFRelease(gpu);
    return avc && hevc ? 0 : 1;
}
