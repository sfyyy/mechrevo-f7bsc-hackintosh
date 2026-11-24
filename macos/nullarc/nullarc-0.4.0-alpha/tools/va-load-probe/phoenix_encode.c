#include "phoenix_encode.h"
#include "phoenix_video_context.h"
#include "phx_avc_io.h"
#include "PhxVideoAVC.h"

#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <VideoToolbox/VTErrors.h>

struct PhoenixVAConnection {
    PhoenixVideoContext context;
    io_connect_t client;
    CMVideoDimensions dimensions;
};

static OSStatus openVideo(PhoenixVAConnection *c, io_service_t accelerator) {
    c->client = PhoenixOpenVideoContext(&c->context, accelerator);
    return c->client ? noErr : kVTVideoEncoderNotAvailableNowErr;
}

OSStatus PhoenixVAOpen(io_service_t accelerator, CMVideoDimensions dimensions, PhoenixVAConnection **out) {
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (!accelerator) return kVTParameterErr;
    PhoenixVAConnection *connection = calloc(1, sizeof(*connection));
    if (!connection) return kVTAllocationFailedErr;
    CFTypeRef abi = IORegistryEntryCreateCFProperty(accelerator, CFSTR(PHX_VIDEO_AVC_PROPERTY), NULL, 0);
    OSStatus status;
    if (abi && dimensions.width == PhxAVCWidth && dimensions.height == PhxAVCHeight) {
        int64_t version = 0;
        status = kVTVideoEncoderNotAvailableNowErr;
        if (CFGetTypeID(abi) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)abi, kCFNumberSInt64Type, &version) &&
            version == PHX_VIDEO_AVC_VERSION)
            status = openVideo(connection, accelerator);
    } else {
        status = PhxAVCOpenForDimensions(dimensions, &connection->client);
    }
    if (abi) CFRelease(abi);
    if (status) {
        PhoenixVAClose(connection);
        return status;
    }
    connection->dimensions = dimensions;
    *out = connection;
    return noErr;
}

void PhoenixVAClose(PhoenixVAConnection *connection) {
    if (!connection) return;
    if (!connection->context.video && connection->client) IOServiceClose(connection->client);
    PhoenixReleaseVideoContext(&connection->context);
    free(connection);
}

static os_log_t encodeLog;
static pthread_once_t logOnce = PTHREAD_ONCE_INIT;

static void openEncodeLog(void) {
    encodeLog = os_log_create("org.nullarc.phoenix.va-load-probe", "encode");
}

static OSStatus copyVideoAccessUnit(io_connect_t client, CVPixelBufferRef image, CFDataRef *accessUnit,
                                    uint8_t digest[CC_SHA256_DIGEST_LENGTH]) {
    uint8_t *packed = malloc(PHXN_VCN_NV12_BYTES + PHXN_VCN_H264_CAPACITY);
    if (!packed) return kVTAllocationFailedErr;
    uint8_t *stream = packed + PHXN_VCN_NV12_BYTES;
    size_t bytes = PHXN_VCN_H264_CAPACITY;
    OSStatus status = PhxAVCPackNV12(image, PhxAVCWidth, PhxAVCHeight, packed, PHXN_VCN_NV12_BYTES);
    if (!status) {
        kern_return_t kr = IOConnectCallStructMethod(client, PHX_VIDEO_ENCODE_AVC,
                                                     packed, PHXN_VCN_NV12_BYTES, stream, &bytes);
        if (kr) {
            os_log_error(encodeLog, "Phoenix video context submission returned %{public}#x", (unsigned)kr);
            status = (kr == kIOReturnNotReady || kr == kIOReturnOffline || kr == kIOReturnBusy ||
                      kr == kIOReturnNoDevice || kr == kIOReturnUnsupported)
                   ? kVTVideoEncoderNotAvailableNowErr : kVTVideoEncoderMalfunctionErr;
        } else if (!bytes || bytes > PHXN_VCN_H264_CAPACITY) {
            status = kVTVideoEncoderMalfunctionErr;
        } else {
            CC_SHA256(packed, PHXN_VCN_NV12_BYTES, digest);
            *accessUnit = CFDataCreate(NULL, stream, (CFIndex)bytes);
            if (!*accessUnit) status = kVTAllocationFailedErr;
        }
    }
    free(packed);
    return status;
}

OSStatus PhoenixVAEncodeFrame(PhoenixVAConnection *connection, CVPixelBufferRef image, CFDataRef *accessUnit) {
    if (!accessUnit) return kVTParameterErr;
    *accessUnit = NULL;
    if (!connection || !image || CVPixelBufferGetWidth(image) != (size_t)connection->dimensions.width ||
        CVPixelBufferGetHeight(image) != (size_t)connection->dimensions.height) return kVTParameterErr;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    pthread_once(&logOnce, openEncodeLog);
    OSStatus status = connection->context.video ? copyVideoAccessUnit(connection->client, image, accessUnit, digest)
        : PhxAVCCopyAnnexBWithDigest(connection->client, image, accessUnit, digest);
    if (status) return status;

    char hex[sizeof(digest) * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[2*i] = digits[digest[i] >> 4];
        hex[2*i + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = '\0';
    os_log(encodeLog, "Phoenix H.264 hardware frame %{public}zux%{public}zu, %{public}ld bytes input_sha256=%{public}s transport=%{public}s",
           CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image), (long)CFDataGetLength(*accessUnit), hex,
           connection->context.video ? "video-context" : "direct");
    return noErr;
}
