#include "phx_avc.h"
#include "phx_vt.h"

#include <stddef.h>
#include <pthread.h>

#if !defined(__x86_64__)
#error The Phoenix codec provider targets the macOS x86_64 VideoToolbox ABI.
#endif

// Version-one implementor tables as used by macOS 26's x86_64 video plug-ins.
// The SDK exports the entry points in its .tbd files, but omits their headers.
typedef struct {
    io_connect_t connection;
    void *session;
    CMVideoDimensions dimensions;
} Encoder;

#pragma pack(push, 4)
typedef struct {
    uint32_t version;
    size_t storageSize;
    void *equal, *invalidate;
    void (*finalize)(CFTypeRef);
    CFStringRef (*debugDescription)(CFTypeRef);
    OSStatus (*copyProperty)(CFTypeRef, CFStringRef, CFAllocatorRef, CFTypeRef *);
    OSStatus (*setProperty)(CFTypeRef, CFStringRef, CFTypeRef);
    void *reserved[4];
} BaseClass;

typedef struct {
    uint32_t version;
    OSStatus (*start)(CFTypeRef, void *, CMVideoDimensions);
    OSStatus (*encode)(CFTypeRef, void *, CVPixelBufferRef, CMTime, CMTime,
                       CFDictionaryRef, VTEncodeInfoFlags *);
    OSStatus (*complete)(CFTypeRef, void *);
    OSStatus (*copySupportedProperties)(CFTypeRef, CFDictionaryRef *);
    void *reserved[17];
} EncoderClass;

typedef struct {
    const void *reserved;
    const BaseClass *base;
    const EncoderClass *encoder;
} EncoderTable;
#pragma pack(pop)

_Static_assert(offsetof(BaseClass, finalize) == 28, "x86_64 CoreMedia layout");
_Static_assert(offsetof(EncoderClass, encode) == 12, "x86_64 VideoToolbox layout");

typedef OSStatus (*Factory)(CMVideoCodecType, CFAllocatorRef, CFTypeRef *);
extern OSStatus VTRegisterVideoEncoderWithInfo(CMVideoCodecType, CFDictionaryRef, Factory);
extern const CFStringRef kVTVideoEncoder_IsHardwareAccelerated;
extern const CFStringRef kVTVideoEncoder_Rating;
extern const CFStringRef kVTVideoEncoder_EncoderNameKey;
extern const void *VTVideoEncoderGetClassID(void);
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const EncoderTable *, const void *, CFTypeRef *);
extern void *CMBaseObjectGetDerivedStorage(CFTypeRef);
extern void VTEncoderSessionSetPixelBufferAttributes(void *, CFDictionaryRef);
extern OSStatus VTEncoderSessionEmitEncodedFrame(void *, void *, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);

static void finalize(CFTypeRef object) {
    Encoder *encoder = CMBaseObjectGetDerivedStorage(object);
    if (encoder->connection) IOServiceClose(encoder->connection);
}

static CFStringRef debugDescription(CFTypeRef object) {
    (void)object;
    return CFRetain(CFSTR("Phoenix VCN AVC encoder"));
}

static OSStatus copyProperty(CFTypeRef object, CFStringRef key, CFAllocatorRef allocator, CFTypeRef *value) {
    (void)allocator;
    if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) {
        Encoder *encoder = CMBaseObjectGetDerivedStorage(object);
        *value = CFRetain(encoder->connection ? kCFBooleanTrue : kCFBooleanFalse);
        return noErr;
    }
    if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) {
        *value = CFRetain(kCFBooleanFalse);
        return noErr;
    }
    return kVTPropertyNotSupportedErr;
}

static OSStatus setProperty(CFTypeRef object, CFStringRef key, CFTypeRef value) {
    (void)object;
    if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering))
        return value == kCFBooleanFalse ? noErr : kVTParameterErr;
    if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder))
        return kVTPropertyReadOnlyErr;
    return kVTPropertyNotSupportedErr;
}

static OSStatus copySupportedProperties(CFTypeRef object, CFDictionaryRef *out) {
    (void)object;
    const void *keys[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
    const void *values[] = {kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadWrite};
    CFDictionaryRef description = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!description) return kVTAllocationFailedErr;
    const void *readOnly[] = {kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly};
    CFDictionaryRef hardware = CFDictionaryCreate(NULL, keys, readOnly, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *properties[] = {kVTCompressionPropertyKey_AllowFrameReordering,
                                kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder};
    const void *descriptions[] = {description, hardware};
    *out = hardware ? CFDictionaryCreate(NULL, properties, descriptions, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    if (hardware) CFRelease(hardware);
    CFRelease(description);
    return *out ? noErr : kVTAllocationFailedErr;
}

static OSStatus start(CFTypeRef object, void *session, CMVideoDimensions dimensions) {
    Encoder *encoder = CMBaseObjectGetDerivedStorage(object);
    if (encoder->session) return kVTInvalidSessionErr;
    const int32_t formats[] = {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                               kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
    CFNumberRef video = CFNumberCreate(NULL, kCFNumberSInt32Type, &formats[0]);
    CFNumberRef full = CFNumberCreate(NULL, kCFNumberSInt32Type, &formats[1]);
    const void *numbers[] = {video, full};
    CFArrayRef supported = video && full ? CFArrayCreate(NULL, numbers, 2, &kCFTypeArrayCallBacks) : NULL;
    if (video) CFRelease(video);
    if (full) CFRelease(full);
    if (!supported) return kVTAllocationFailedErr;
    const void *key = kCVPixelBufferPixelFormatTypeKey, *value = supported;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(supported);
    if (!attrs) return kVTAllocationFailedErr;
    OSStatus status = PhxAVCOpenForDimensions(dimensions, &encoder->connection);
    if (status == noErr) {
        encoder->session = session;
        encoder->dimensions = dimensions;
        VTEncoderSessionSetPixelBufferAttributes(session, attrs);
    }
    CFRelease(attrs);
    return status;
}

static OSStatus encode(CFTypeRef object, void *sourceFrame, CVPixelBufferRef pixel, CMTime pts,
                       CMTime duration, CFDictionaryRef properties, VTEncodeInfoFlags *flags) {
    (void)properties;
    Encoder *encoder = CMBaseObjectGetDerivedStorage(object);
    if (!encoder->session) return kVTInvalidSessionErr;
    if (flags) *flags = 0;
    if (!pixel || CVPixelBufferGetWidth(pixel) != (size_t)encoder->dimensions.width ||
        CVPixelBufferGetHeight(pixel) != (size_t)encoder->dimensions.height) return kVTParameterErr;
    CMSampleBufferRef sample = NULL;
    OSStatus status = PhxAVCEncode(encoder->connection, pixel, pts, duration, &sample);
    if (status == noErr) {
        if (!sample) return kVTVideoEncoderMalfunctionErr;
        status = VTEncoderSessionEmitEncodedFrame(encoder->session, sourceFrame, noErr, 0, sample);
    }
    if (sample) CFRelease(sample);
    return status;
}

static OSStatus complete(CFTypeRef object, void *sourceFrame) {
    (void)object;
    (void)sourceFrame;
    return noErr;
}

// Each packed class begins with a four-byte version. Position that word four
// bytes after an aligned address so dyld can bind the following pointers.
static const struct {
    uint32_t padding;
    BaseClass base;
} __attribute__((aligned(8))) baseStorage = {
    .base = {.version = 1, .storageSize = sizeof(Encoder), .finalize = finalize,
             .debugDescription = debugDescription, .copyProperty = copyProperty, .setProperty = setProperty}
};
static const struct {
    uint32_t padding;
    EncoderClass encoder;
} __attribute__((aligned(8))) classStorage = {
    .encoder = {.version = 1, .start = start, .encode = encode, .complete = complete,
                .copySupportedProperties = copySupportedProperties}
};
static const EncoderTable encoderTable = {NULL, &baseStorage.base, &classStorage.encoder};

// CoreMedia resolves this factory by name from the installed bundle's Info.plist.
OSStatus PhxAVCEncoderCreate(CMVideoCodecType codec, CFAllocatorRef allocator, CFTypeRef *result) {
    *result = NULL;
    if (codec != kCMVideoCodecType_H264) return kVTCouldNotFindVideoEncoderErr;
    return CMDerivedObjectCreate(allocator, &encoderTable, VTVideoEncoderGetClassID(), result);
}

static pthread_mutex_t registrationLock = PTHREAD_MUTEX_INITIALIZER;
static bool registered;

OSStatus PhxRegisterAVCEncoder(void) {
    pthread_mutex_lock(&registrationLock);
    OSStatus status = noErr;
    if (!registered) {
        io_connect_t probe = IO_OBJECT_NULL;
        status = PhxAVCOpen(&probe);
        if (status == noErr) {
            IOServiceClose(probe);
            // In-process callers explicitly request the encoder ID.
            int32_t priority = -100;
            CFNumberRef rating = CFNumberCreate(NULL, kCFNumberSInt32Type, &priority);
            const void *keys[] = {CFSTR("CMClassImplementationID"), kVTVideoEncoder_EncoderNameKey,
                                  kVTVideoEncoder_IsHardwareAccelerated, kVTVideoEncoder_Rating};
            const void *values[] = {PhxAVCEncoderID, CFSTR("Phoenix VCN H.264"), kCFBooleanTrue, rating};
            CFDictionaryRef info = rating ? CFDictionaryCreate(NULL, keys, values, 4,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
            status = info ? VTRegisterVideoEncoderWithInfo(kCMVideoCodecType_H264, info, PhxAVCEncoderCreate)
                          : kVTAllocationFailedErr;
            if (info) CFRelease(info);
            if (rating) CFRelease(rating);
            registered = status == noErr;
        }
    }
    pthread_mutex_unlock(&registrationLock);
    return status;
}
