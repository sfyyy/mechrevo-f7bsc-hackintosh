#include "phoenix_decode.h"
#include "phoenix_layout.h"
#include "phx_preferences.h"
#include "phoenix_video_context.h"
#include "PhxVideoDecode.h"

#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VTErrors.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>

struct PhoenixVADecoder {
    PhoenixVideoContext context;
    io_connect_t client;
    uint64_t registryID;
    uint64_t streamFrames;
    CMVideoDimensions dimensions;
    bool reference, stream;
    uint8_t nv12[];
};

static bool hasVersion(io_service_t service, CFStringRef key, int64_t wanted) {
    if (!service) return false;
    CFTypeRef abi = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int64_t version = 0;
    bool supported = abi && CFGetTypeID(abi) == CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)abi, kCFNumberSInt64Type, &version) && version == wanted;
    if (abi) CFRelease(abi);
    return supported;
}

bool PhoenixVADecodeStreamAvailable(io_service_t service) {
    return hasVersion(service, CFSTR(PHX_VIDEO_DECODE_STREAM_PROPERTY), PHX_VIDEO_DECODE_STREAM_VERSION);
}

bool PhoenixVADecodeHasStream(const PhoenixVADecoder *decoder) {
    return decoder && decoder->stream;
}

bool PhoenixVADecodeHasReference(const PhoenixVADecoder *decoder) {
    return decoder && decoder->reference;
}

bool PhoenixVADecodeAvailable(io_service_t service) {
    return hasVersion(service, CFSTR(PHX_VIDEO_DECODE_PROPERTY), PHX_VIDEO_DECODE_VERSION) ||
           PhoenixVADecodeStreamAvailable(service);
}

static bool streamSize(CMVideoDimensions d) {
    return d.width >= 64 && d.height >= 64 && (uint32_t)d.width <= PHX_VIDEO_DECODE_STREAM_MAX_WIDTH &&
        (uint32_t)d.height <= PHX_VIDEO_DECODE_STREAM_MAX_HEIGHT && !((d.width | d.height) & 1);
}

OSStatus PhoenixVADecodeOpen(io_service_t service, CMVideoDimensions dimensions, PhoenixVADecoder **out) {
    if (!out) return kVTParameterErr;
    *out = NULL;
    const bool fixedSize = dimensions.width == PHX_VIDEO_DECODE_WIDTH && dimensions.height == PHX_VIDEO_DECODE_HEIGHT;
    const bool stream = PhoenixVADecodeStreamAvailable(service);
    if (!fixedSize && !(stream && streamSize(dimensions))) return kVTVideoDecoderUnsupportedDataFormatErr;
    const bool reference = fixedSize && hasVersion(service, CFSTR(PHX_VIDEO_DECODE_PROPERTY), PHX_VIDEO_DECODE_VERSION);
    if (!stream && !reference) return kVTVideoDecoderNotAvailableNowErr;
    size_t capacity = (size_t)dimensions.width * dimensions.height * 3 / 2;
    if (reference && capacity < PHX_VIDEO_DECODE_OUTPUT_BYTES) capacity = PHX_VIDEO_DECODE_OUTPUT_BYTES;
    PhoenixVADecoder *decoder = calloc(1, sizeof(*decoder) + capacity);
    if (!decoder) return kVTAllocationFailedErr;
    decoder->dimensions = dimensions;
    decoder->stream = stream;
    decoder->reference = reference;
    decoder->client = PhoenixOpenVideoContext(&decoder->context, service);
    if (!decoder->client) {
        PhoenixVADecodeClose(decoder);
        return kVTVideoDecoderNotAvailableNowErr;
    }
    IORegistryEntryGetRegistryEntryID(service, &decoder->registryID);
    *out = decoder;
    return noErr;
}

void PhoenixVADecodeClose(PhoenixVADecoder *decoder) {
    if (!decoder) return;
    if (decoder->streamFrames)
        os_log(OS_LOG_DEFAULT, "Phoenix AVC stream closed registry=%{public}llu frames=%{public}llu",
               (unsigned long long)decoder->registryID, (unsigned long long)decoder->streamFrames);
    PhoenixReleaseVideoContext(&decoder->context);
    free(decoder);
}

// Accept both Annex B prefix lengths. A decoder at this ABI version only
// accepts the five IDR slices of the reference picture. The kernel compares
// every normalized byte against the supported stream before it touches VCN.
static bool packSlices(const uint8_t *data, size_t length, uint8_t packed[PHX_VIDEO_DECODE_INPUT_BYTES]) {
    if (!data || !length || length > PHX_VIDEO_DECODE_INPUT_BYTES + 5) return false;
    size_t pos = 0, used = 0;
    unsigned slices = 0;
    while (pos < length) {
        size_t zeros = 0;
        while (pos < length && !data[pos]) { ++pos; ++zeros; }
        if ((zeros != 2 && zeros != 3) || pos >= length || data[pos++] != 1) return false;
        size_t start = pos;
        while (pos < length) {
            if (pos + 2 < length && !data[pos] && !data[pos + 1] &&
                (data[pos + 2] == 1 || (!data[pos + 2] && pos + 3 < length && data[pos + 3] == 1))) break;
            ++pos;
        }
        size_t n = pos - start;
        if (n < 2 || (data[start] & 0x9f) != 5 || !((data[start] >> 5) & 3) ||
            PHX_VIDEO_DECODE_INPUT_BYTES - used < 3 || n > PHX_VIDEO_DECODE_INPUT_BYTES - used - 3) return false;
        const uint8_t prefix[3] = {0, 0, 1};
        memcpy(packed + used, prefix, sizeof(prefix));
        memcpy(packed + used + 3, data + start, n);
        used += 3 + n;
        ++slices;
    }
    return slices == 5 && used == PHX_VIDEO_DECODE_INPUT_BYTES;
}

static bool matchesDimension(size_t extent, size_t visible) {
    return extent == visible || extent == ((visible + 15u) & ~(size_t)15u);
}

// PHX_DIRECT_ANY_OFFSET=0, or DirectAnyOffset=false in com.phoenix.va, restores
// the build 75 rule that wanted the chroma plane right after the visible rows.
static bool directAnyOffsetEnabled(void) {
    static int cached = -1;
    const int known = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (known >= 0) return known != 0;
    const bool enabled = phxPreferenceBool("PHX_DIRECT_ANY_OFFSET", CFSTR("DirectAnyOffset"), true);
    __atomic_store_n(&cached, enabled ? 1 : 0, __ATOMIC_RELAXED);
    return enabled;
}

static OSStatus submitFrame(PhoenixVADecoder *decoder, uint32_t selector, const uint8_t *input, size_t length,
                            CMVideoDimensions visible, size_t sourcePitch, CVPixelBufferRef destination) {
    const size_t width = (size_t)visible.width, height = (size_t)visible.height;
    const size_t surfaceWidth = CVPixelBufferGetWidth(destination), surfaceHeight = CVPixelBufferGetHeight(destination);
    // Both variants are the same 8-bit bi-planar 420 layout; the only difference is
    // the range tag the renderer reads. A full-range stream (JPEG range, as several
    // live encoders emit) makes the caller ask for the full-range variant, so
    // accepting only video range silently dropped every frame of those streams.
    const OSType format = CVPixelBufferGetPixelFormatType(destination);
    const bool fullRange = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    if ((!fullRange && format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ||
        !matchesDimension(surfaceWidth, width) || !matchesDimension(surfaceHeight, height) ||
        CVPixelBufferGetPlaneCount(destination) != 2) {
        // The caller owns this surface and the format decides whether the picture
        // is video or full range; a mismatch here is invisible without the value.
        os_log(OS_LOG_DEFAULT,
               "Phoenix decode surface rejected format=%{public}c%{public}c%{public}c%{public}c %{public}zux%{public}zu"
               " planes=%{public}zu visible=%{public}zux%{public}zu",
               (char)((CVPixelBufferGetPixelFormatType(destination) >> 24) & 0xff),
               (char)((CVPixelBufferGetPixelFormatType(destination) >> 16) & 0xff),
               (char)((CVPixelBufferGetPixelFormatType(destination) >> 8) & 0xff),
               (char)(CVPixelBufferGetPixelFormatType(destination) & 0xff),
               surfaceWidth, surfaceHeight, (size_t)CVPixelBufferGetPlaneCount(destination), width, height);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    if (CVPixelBufferLockBaseAddress(destination, 0) != kCVReturnSuccess) return kVTVideoDecoderMalfunctionErr;
    OSStatus status = noErr;
    uint8_t *planes[2];
    size_t pitches[2];
    for (size_t i = 0; i < 2; ++i) {
        planes[i] = CVPixelBufferGetBaseAddressOfPlane(destination, i);
        pitches[i] = CVPixelBufferGetBytesPerRowOfPlane(destination, i);
        if (!planes[i] || pitches[i] < surfaceWidth ||
            CVPixelBufferGetHeightOfPlane(destination, i) != (surfaceHeight >> i) ||
            CVPixelBufferGetWidthOfPlane(destination, i) != (surfaceWidth >> i)) status = kVTParameterErr;
    }
    // Both planes of a surface are normally one allocation at a single stride, so the
    // kernel can write the decoded picture straight into it and nothing is copied here.
    // Only the stream selector carries the layout; the reference method has a fixed shape.
    const bool stream = selector == PHX_VIDEO_DECODE_STREAM_AVC;
    const PhoenixSurfaceLayout layout = phoenixSurfaceLayout(
        stream && !status, surfaceWidth, surfaceHeight, planes[0], pitches[0], planes[1], pitches[1],
        CVPixelBufferGetBaseAddress(destination), CVPixelBufferGetDataSize(destination),
        directAnyOffsetEnabled());
    const bool direct = layout.direct;
    const size_t chromaOffset = layout.chromaOffset;
    const bool logLayout = stream && decoder->streamFrames == 0;
    uint8_t *target = direct ? planes[0] : decoder->nv12;
    // The reference method keeps its fixed PITCH-strided layout; the stream fallback is packed.
    const size_t bounceOut = sourcePitch * height * 3 / 2;
    size_t capacity = direct ? chromaOffset + pitches[0] * (surfaceHeight >> 1) : bounceOut;
    if (!status) {
        const uint64_t layoutWords[2] = {direct ? pitches[0] : 0, direct ? chromaOffset : 0};
        kern_return_t kr = stream ?
            IOConnectCallMethod(decoder->client, selector, layoutWords, 2, input, length,
                                NULL, NULL, target, &capacity) :
            IOConnectCallStructMethod(decoder->client, selector, input, length, target, &capacity);
        const size_t expected = direct ? chromaOffset + pitches[0] * (surfaceHeight >> 1) : bounceOut;
        if (kr || capacity != expected) {
#if PHX_REFERENCE_DECODE_PROBE
            os_log(OS_LOG_DEFAULT, "Phoenix decode IO selector=%{public}#x result=%{public}#x bytes=%{public}zu expected=%{public}zu direct=%{public}d",
                   selector, (unsigned)kr, capacity, expected, (int)direct);
#else
            os_log_debug(OS_LOG_DEFAULT, "Phoenix decode registry=%{public}llu IOReturn=%{public}#x output_bytes=%{public}zu",
                         (unsigned long long)decoder->registryID, (unsigned)kr, capacity);
#endif
        }
        if (kr == kIOReturnNotReady || kr == kIOReturnOffline || kr == kIOReturnBusy || kr == kIOReturnNoDevice)
            status = kVTVideoDecoderNotAvailableNowErr;
        else if (kr == kIOReturnUnsupported || kr == kIOReturnBadArgument) status = kVTVideoDecoderUnsupportedDataFormatErr;
        else if (kr || capacity != expected) status = kVTVideoDecoderMalfunctionErr;
    }
    if (!status && logLayout)
        os_log(OS_LOG_DEFAULT, "Phoenix AVC layout direct=%{public}d pitch=%{public}zu chroma=%{public}zu data=%{public}zu",
               (int)direct, pitches[0], chromaOffset, CVPixelBufferGetDataSize(destination));
    if (!status && !direct) {
        const uint8_t *src = decoder->nv12;
        for (size_t i = 0; i < 2; ++i) {
            uint8_t *dst = planes[i];
            const int black = i ? 128 : (fullRange ? 0 : 16);
            for (size_t y = 0; y < (height >> i); ++y, src += sourcePitch, dst += pitches[i]) {
                memcpy(dst, src, width);
                if (surfaceWidth > width) memset(dst + width, black, surfaceWidth - width);
            }
            for (size_t y = height >> i; y < (surfaceHeight >> i); ++y, dst += pitches[i])
                memset(dst, black, surfaceWidth);
        }
    }
    if (CVPixelBufferUnlockBaseAddress(destination, 0) != kCVReturnSuccess) status = kVTVideoDecoderMalfunctionErr;
    if (!status && selector == PHX_VIDEO_DECODE_AVC)
        os_log(OS_LOG_DEFAULT, "Phoenix reference IDR completed registry=%{public}llu output_bytes=%{public}zu (864x480 NV12)",
               (unsigned long long)decoder->registryID, capacity);
    return status;
}

OSStatus PhoenixVADecodeFrame(PhoenixVADecoder *decoder, const uint8_t *annexB, size_t length,
                              CVPixelBufferRef destination) {
    if (!decoder || !destination || !annexB) return kVTParameterErr;
    uint8_t slices[PHX_VIDEO_DECODE_INPUT_BYTES];
    if (!decoder->reference || !packSlices(annexB, length, slices)) return kVTVideoDecoderUnsupportedDataFormatErr;
    return submitFrame(decoder, PHX_VIDEO_DECODE_AVC, slices, sizeof(slices), decoder->dimensions,
                       PHX_VIDEO_DECODE_PITCH, destination);
}

OSStatus PhoenixVADecodeAccessUnit(PhoenixVADecoder *decoder, const uint8_t *annexB, size_t length,
                                   CMVideoDimensions visible, CVPixelBufferRef destination) {
    if (!decoder || !destination || !annexB) return kVTParameterErr;
    if (!decoder->stream || !streamSize(visible) || length < 5 || length > PHX_VIDEO_DECODE_STREAM_MAX_INPUT ||
        !matchesDimension(decoder->dimensions.width, visible.width) ||
        !matchesDimension(decoder->dimensions.height, visible.height)) return kVTVideoDecoderUnsupportedDataFormatErr;
    OSStatus status = submitFrame(decoder, PHX_VIDEO_DECODE_STREAM_AVC, annexB, length, visible, visible.width, destination);
    if (!status) {
        uint64_t frames = ++decoder->streamFrames;
        if (frames == 1 || frames % 60 == 0)
            os_log(OS_LOG_DEFAULT, "Phoenix AVC stream completed registry=%{public}llu frames=%{public}llu visible=%{public}dx%{public}d",
                   (unsigned long long)decoder->registryID, (unsigned long long)frames, visible.width, visible.height);
    }
    return status;
}
