// Process-local reference-frame decoder. Version-one implementor layouts follow
// WebKit's VTVideoDecoderSPI.h and CMBaseObjectSPI.h (c1f317a372bd04b2ca1b6390a8fc0753f5fd162b).
#include "phoenix_decode.h"
#include "PhxVideoDecode.h"
#include <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(__x86_64__)
#error This decoder implements the x86_64 macOS ABI.
#endif
#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    size_t size;
    void *equal;
    OSStatus (*invalidate)(CFTypeRef);
    void (*finalize)(CFTypeRef);
    void *description;
    OSStatus (*copyProperty)(CFTypeRef, CFStringRef, CFAllocatorRef, CFTypeRef *);
    void *setProperty, *barrier, *protocols;
} BaseClass;
typedef struct {
    uint32_t version;
    OSStatus (*start)(CFTypeRef, void *, CMVideoFormatDescriptionRef);
    OSStatus (*decode)(CFTypeRef, void *, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    void *optional[8];
} DecoderClass;
#pragma pack(pop)
typedef struct { const void *reserved; const BaseClass *base; const DecoderClass *decoder; } Table;
_Static_assert(offsetof(DecoderClass, decode) == 12 && offsetof(BaseClass, finalize) == 28, "x86_64 class layout");
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const Table *, const void *, CFTypeRef *);
extern const void *VTVideoDecoderGetClassID(void);
extern void *CMBaseObjectGetDerivedStorage(CFTypeRef);
extern void VTDecoderSessionEmitDecodedFrame(void *, void *, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(void *, CFDictionaryRef);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(void *);
typedef OSStatus (*Factory)(CMVideoCodecType, CFAllocatorRef, CFTypeRef *);
extern OSStatus VTRegisterVideoDecoderWithInfo(CMVideoCodecType, CFDictionaryRef, Factory);
extern const CFStringRef kVTVideoDecoderSpecification_DecoderID;
extern const CFStringRef kVTVideoDecoder_IsHardwareAccelerated;
extern const CFStringRef kVTVideoDecoderInfo_Rating;
#define DecoderID CFSTR("org.nullarc.phoenix.decode.reference.local")

typedef struct {
    pthread_mutex_t lock;
    bool initialized;
    void *session;
    PhoenixVADecoder *transport;
} Decoder;
static atomic_uint completedSubmissions;
unsigned PhxVTReferenceFrames(void) { return atomic_load(&completedSubmissions); }

// The kernel's fixed firmware picture and scaling tables only describe this pair.
static bool referenceFormat(CMVideoFormatDescriptionRef format, int *prefixSize) {
    static const uint8_t sps[] = {0x67,0x64,0x00,0x1e,0xac,0x52,0x30,0x36,0x0f,0x64};
    static const uint8_t pps[] = {0x68,0xde,0x09,0xc8};
    if (!format || CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_H264) return false;
    CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(format);
    if (d.width != PHX_VIDEO_DECODE_WIDTH || d.height != PHX_VIDEO_DECODE_HEIGHT) return false;
    const uint8_t *bytes = NULL;
    size_t len = 0, count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &bytes, &len, &count, prefixSize) ||
        count != 2 || !(*prefixSize == 1 || *prefixSize == 2 || *prefixSize == 4) ||
        len != sizeof(sps) || memcmp(bytes, sps, len)) return false;
    return !CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &bytes, &len, NULL, NULL) &&
        len == sizeof(pps) && !memcmp(bytes, pps, len);
}

static OSStatus invalidate(CFTypeRef object) {
    Decoder *d = CMBaseObjectGetDerivedStorage(object);
    if (d->initialized) {
        pthread_mutex_lock(&d->lock);
        PhoenixVADecodeClose(d->transport);
        d->transport = NULL;
        d->session = NULL;
        pthread_mutex_unlock(&d->lock);
    }
    return noErr;
}
static void finalize(CFTypeRef object) {
    Decoder *d = CMBaseObjectGetDerivedStorage(object);
    invalidate(object);
    if (d->initialized) pthread_mutex_destroy(&d->lock);
}

static OSStatus copyProperty(CFTypeRef object, CFStringRef key, CFAllocatorRef allocator, CFTypeRef *out) {
    (void)allocator;
    if (!out || !key) return kVTParameterErr;
    *out = NULL;
    if (!CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) return kVTPropertyNotSupportedErr;
    Decoder *d = CMBaseObjectGetDerivedStorage(object);
    pthread_mutex_lock(&d->lock);
    CFBooleanRef hardware = kCFBooleanFalse;
#ifndef PHX_TEST_SOFTWARE_TRANSPORT
    if (d->transport) hardware = kCFBooleanTrue;
#endif
    *out = CFRetain(hardware);
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

static OSStatus setAttributes(void *session, CMVideoDimensions dims) {
    int32_t format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef width = CFNumberCreate(NULL, kCFNumberSInt32Type, &dims.width);
    CFNumberRef height = CFNumberCreate(NULL, kCFNumberSInt32Type, &dims.height);
    CFNumberRef nv12 = CFNumberCreate(NULL, kCFNumberSInt32Type, &format);
    const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey};
    const void *values[] = {width, height, nv12};
    CFDictionaryRef attrs = width && height && nv12 ? CFDictionaryCreate(NULL, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    OSStatus status = attrs ? VTDecoderSessionSetPixelBufferAttributes(session, attrs) : kVTAllocationFailedErr;
    if (attrs) CFRelease(attrs);
    if (nv12) CFRelease(nv12);
    if (width) CFRelease(width);
    if (height) CFRelease(height);
    return status;
}

static OSStatus start(CFTypeRef object, void *session, CMVideoFormatDescriptionRef format) {
    int prefixSize;
    if (!referenceFormat(format, &prefixSize)) return kVTVideoDecoderUnsupportedDataFormatErr;
    Decoder *d = CMBaseObjectGetDerivedStorage(object);
    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    pthread_mutex_lock(&d->lock);
    OSStatus status = d->transport ? kVTInvalidSessionErr : kVTVideoDecoderNotAvailableNowErr;
    if (service && !d->transport) {
        status = PhoenixVADecodeOpen(service, dims, &d->transport);
        if (!status) {
            status = setAttributes(session, dims);
            if (status) { PhoenixVADecodeClose(d->transport); d->transport = NULL; }
            else d->session = session;
        }
    }
    pthread_mutex_unlock(&d->lock);
    if (service) IOObjectRelease(service);
    fprintf(stderr, "project reference decoder start: %d\n", (int)status);
    return status;
}

static OSStatus slicesFromSample(CMSampleBufferRef sample, uint8_t *slices) {
    int prefixSize;
    if (!sample || CMSampleBufferGetNumSamples(sample) != 1 || !CMSampleBufferDataIsReady(sample) ||
        !referenceFormat(CMSampleBufferGetFormatDescription(sample), &prefixSize)) return kVTVideoDecoderUnsupportedDataFormatErr;
    CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
    const size_t payload = PHX_VIDEO_DECODE_INPUT_BYTES - 5 * 3;
    const size_t total = payload + 5 * (size_t)prefixSize;
    if (!data || CMSampleBufferGetSampleSize(sample, 0) != total || CMBlockBufferGetDataLength(data) != total)
        return kVTVideoDecoderBadDataErr;
    size_t pos = 0, used = 0;
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t header[4];
        if (total-pos < (size_t)prefixSize || CMBlockBufferCopyDataBytes(data, pos, (size_t)prefixSize, header))
            return kVTVideoDecoderBadDataErr;
        size_t length = 0;
        for (int n = 0; n < prefixSize; ++n) length = length * 256 + header[n];
        pos += (size_t)prefixSize;
        if (length < 2 || length > total-pos || PHX_VIDEO_DECODE_INPUT_BYTES-used < 3 ||
            length > PHX_VIDEO_DECODE_INPUT_BYTES-used-3) return kVTVideoDecoderBadDataErr;
        const uint8_t prefix[3] = {0,0,1};
        memcpy(slices+used, prefix, 3);
        used += 3;
        if (CMBlockBufferCopyDataBytes(data, pos, length, slices+used) || (slices[used] & 0x9f) != 5)
            return kVTVideoDecoderBadDataErr;
        used += length;
        pos += length;
    }
    return used == PHX_VIDEO_DECODE_INPUT_BYTES && pos == total ? noErr : kVTVideoDecoderBadDataErr;
}

static OSStatus decode(CFTypeRef object, void *frame, CMSampleBufferRef sample,
                       VTDecodeFrameFlags flags, VTDecodeInfoFlags *info) {
    if (info) *info = 0;
    uint8_t slices[PHX_VIDEO_DECODE_INPUT_BYTES];
    OSStatus status = slicesFromSample(sample, slices);
    Decoder *d = CMBaseObjectGetDerivedStorage(object);
    pthread_mutex_lock(&d->lock);
    void *session = d->session;
    CVPixelBufferRef pixel = NULL;
    if (!status) {
        CVPixelBufferPoolRef pool = session ? VTDecoderSessionGetPixelBufferPool(session) : NULL;
        status = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixel) : kVTInvalidSessionErr;
    }
    if (!status) status = PhoenixVADecodeFrame(d->transport, slices, sizeof(slices), pixel);
    pthread_mutex_unlock(&d->lock);
    if (!status) atomic_fetch_add(&completedSubmissions, 1);
    if (session) {
        // VT tracks every frame token, including rejected submissions. Retire
        // errors with their status and no image so draining the session finishes.
        bool suppress = !status && (flags & kVTDecodeFrame_DoNotOutputFrame);
        VTDecoderSessionEmitDecodedFrame(session, frame, status, suppress ? kVTDecodeInfo_FrameDropped : 0,
                                         (status || suppress) ? NULL : pixel);
    }
    if (pixel) CFRelease(pixel);
    return status;
}

static const struct { uint32_t alignment; BaseClass data; } __attribute__((aligned(8))) base = {
    .data = {.version=1, .size=sizeof(Decoder), .invalidate=invalidate, .finalize=finalize, .copyProperty=copyProperty}
};
static const struct { uint32_t alignment; DecoderClass data; } __attribute__((aligned(8))) cls = {
    .data = {.version=1, .start=start, .decode=decode}
};
static const Table table = {NULL, &base.data, &cls.data};

static OSStatus factory(CMVideoCodecType codec, CFAllocatorRef allocator, CFTypeRef *out) {
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (codec != kCMVideoCodecType_H264) return kVTCouldNotFindVideoDecoderErr;
    OSStatus status = CMDerivedObjectCreate(allocator, &table, VTVideoDecoderGetClassID(), out);
    if (!status) {
        Decoder *d = CMBaseObjectGetDerivedStorage(*out);
        if (pthread_mutex_init(&d->lock, NULL)) { CFRelease(*out); *out = NULL; return kVTAllocationFailedErr; }
        d->initialized = true;
    }
    return status;
}

OSStatus PhxRegisterVTReference(CFDictionaryRef *spec) {
    *spec = NULL;
    int32_t rating = -100;
    CFNumberRef priority = CFNumberCreate(NULL, kCFNumberSInt32Type, &rating);
    const void *keys[] = {CFSTR("CMClassImplementationID"), kVTVideoDecoderInfo_Rating, kVTVideoDecoder_IsHardwareAccelerated};
#ifdef PHX_TEST_SOFTWARE_TRANSPORT
    const void *hardware = kCFBooleanFalse;
#else
    const void *hardware = kCFBooleanTrue;
#endif
    const void *values[] = {DecoderID, priority, hardware};
    CFDictionaryRef info = priority ? CFDictionaryCreate(NULL, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    if (priority) CFRelease(priority);
    if (!info) return kVTAllocationFailedErr;
    OSStatus status = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, factory);
    CFRelease(info);
    if (status) return status;
    // VTRegisterVideoDecoderWithInfo appends the codec tag to a supplied ID.
    const void *key = kVTVideoDecoderSpecification_DecoderID, *value = CFSTR("org.nullarc.phoenix.decode.reference.local.avc1");
    *spec = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return *spec ? noErr : kVTAllocationFailedErr;
}
