// Diagnostic transport for the x86_64 AppleGVA video-context path. The first
// submit below is the framework's zero-work buffer allocation handshake.
// A staged picture has no hardware completion and must fail the encoder call.
#include "phoenix_encode.h"
#include "phx_avc_io.h"
#include <CommonCrypto/CommonDigest.h>
#include <VideoToolbox/VTErrors.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct __IOAccelDevice *Device;
typedef struct __IOAccelShared *Shared;
typedef struct __IOAccelVideoContext *Video;
typedef struct __IOAccelResource *Resource;

// IOAccelerator on this machine, as used by AMDRadeonVADriver2::VAContext::Init.
extern Device IOAccelDeviceCreate(io_service_t accelerator);
extern Shared IOAccelSharedCreate(Device device);
extern Video IOAccelVideoContextCreate(Shared shared);
extern io_connect_t IOAccelVideoContextGetConnect(Video video);
extern kern_return_t IOAccelVideoContextGetSidebandBuffer(Video video, mach_vm_address_t *address,
                                                          mach_vm_size_t *length);
extern unsigned IOAccelVideoContextGetDataBufferClassCount(Video video);
extern void IOAccelVideoContextSetDataBufferClassUsedBytes(Video video, unsigned index, size_t used);
extern kern_return_t IOAccelVideoContextSubmitDataBuffers(Video video, unsigned used, uint32_t *flags,
                                                          uint32_t *stamp);
extern Resource IOAccelVideoContextGetDataBufferResource(Video video, unsigned index);
extern size_t IOAccelResourceGetDataSize(Resource resource);
extern void *IOAccelResourceGetDataBytes(Resource resource);
extern void IOAccelVideoContextRelease(Video video);
extern void IOAccelSharedRelease(Shared shared);
extern void IOAccelDeviceRelease(Device device);

struct PhoenixVAConnection {
    Device device;
    Shared shared;
    Video video;
    mach_vm_address_t sideband;
    os_log_t log;
};

void PhoenixVAClose(PhoenixVAConnection *c) {
    if (!c) return;
    if (c->sideband)
        IOConnectUnmapMemory64(IOAccelVideoContextGetConnect(c->video), 0, mach_task_self(), c->sideband);
    if (c->video) IOAccelVideoContextRelease(c->video);
    if (c->shared) IOAccelSharedRelease(c->shared);
    if (c->device) IOAccelDeviceRelease(c->device);
    os_release(c->log);
    free(c);
}

OSStatus PhoenixVAOpen(io_service_t accelerator, CMVideoDimensions dimensions, PhoenixVAConnection **out) {
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (!accelerator || dimensions.width != PhxAVCWidth || dimensions.height != PhxAVCHeight)
        return kVTParameterErr;
    PhoenixVAConnection *c = calloc(1, sizeof(*c));
    if (!c) return kVTAllocationFailedErr;
    c->log = os_log_create("org.nullarc.phoenix.va-load-probe", "video-context");
    c->device = IOAccelDeviceCreate(accelerator);
    if (c->device) c->shared = IOAccelSharedCreate(c->device);
    if (c->shared) c->video = IOAccelVideoContextCreate(c->shared);
    OSStatus status = kVTVideoEncoderNotAvailableNowErr;
    if (!c->video) goto failed;
    mach_vm_size_t sidebandBytes = 0;
    kern_return_t kr = IOAccelVideoContextGetSidebandBuffer(c->video, &c->sideband, &sidebandBytes);
    if (kr || !c->sideband || !sidebandBytes) goto failed;
    unsigned classes = IOAccelVideoContextGetDataBufferClassCount(c->video);
    if (classes != 16) goto failed;
    for (unsigned i = 0; i < classes; ++i) IOAccelVideoContextSetDataBufferClassUsedBytes(c->video, i, 0);
    uint32_t flags = 0, stamp = 0;
    kr = IOAccelVideoContextSubmitDataBuffers(c->video, 0, &flags, &stamp);
    if (kr) goto failed;
    classes = IOAccelVideoContextGetDataBufferClassCount(c->video);
    if (classes != 2) goto failed;
    Resource commands = IOAccelVideoContextGetDataBufferResource(c->video, 0);
    Resource data = IOAccelVideoContextGetDataBufferResource(c->video, 1);
    if (!commands || !data) goto failed;
    size_t commandBytes = IOAccelResourceGetDataSize(commands);
    size_t dataBytes = IOAccelResourceGetDataSize(data);
    if (!commandBytes || dataBytes < PhxAVCInputBytes || !IOAccelResourceGetDataBytes(data)) goto failed;
    os_log(c->log, "Video context ready sideband=%{public}llu command=%{public}zu data=%{public}zu",
           sidebandBytes, commandBytes, dataBytes);
    *out = c;
    return noErr;
failed:
    PhoenixVAClose(c);
    return status;
}

OSStatus PhoenixVAEncodeFrame(PhoenixVAConnection *c, CVPixelBufferRef image, CFDataRef *accessUnit) {
    if (!accessUnit) return kVTParameterErr;
    *accessUnit = NULL;
    if (!c || !image) return kVTParameterErr;
    Resource data = IOAccelVideoContextGetDataBufferResource(c->video, 1);
    if (!data) return kVTVideoEncoderNotAvailableNowErr;
    uint8_t *packed = IOAccelResourceGetDataBytes(data);
    OSStatus status = PhxAVCPackNV12(image, PhxAVCWidth, PhxAVCHeight, packed, IOAccelResourceGetDataSize(data));
    if (status) return status;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    char hex[sizeof(digest) * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    CC_SHA256(packed, PhxAVCInputBytes, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = '\0';
    os_log(c->log, "Video context staged NV12 %{public}d x %{public}d bytes=%{public}u sha256=%{public}s",
           PhxAVCWidth, PhxAVCHeight, (unsigned)PhxAVCInputBytes, hex);
    IOAccelVideoContextSetDataBufferClassUsedBytes(c->video, 0, 0);
    IOAccelVideoContextSetDataBufferClassUsedBytes(c->video, 1, PhxAVCInputBytes);
    uint32_t flags = 0, stamp = 0;
    kern_return_t kr = IOAccelVideoContextSubmitDataBuffers(c->video, 0, &flags, &stamp);
    os_log(c->log, "Video context submitted NV12 bytes=%{public}u sha256=%{public}s kr=%{public}x",
           (unsigned)PhxAVCInputBytes, hex, (unsigned)kr);
    // The family may recycle the submitted resource immediately. Look at the
    // next allocation only through a fresh resource reference. A repeated
    // payload tests mapping reuse, not writeback by hardware.
    if (kr == KERN_SUCCESS) {
        Resource next = IOAccelVideoContextGetDataBufferResource(c->video, 1);
        size_t nextBytes = next ? IOAccelResourceGetDataSize(next) : 0;
        const void *nextData = next ? IOAccelResourceGetDataBytes(next) : NULL;
        uint8_t nextDigest[CC_SHA256_DIGEST_LENGTH];
        bool preserved = nextData && nextBytes >= PhxAVCInputBytes &&
                         CC_SHA256(nextData, PhxAVCInputBytes, nextDigest) &&
                         memcmp(nextDigest, digest, sizeof(digest)) == 0;
        os_log(c->log, "Video context next NV12 resource bytes=%{public}zu input_preserved=%{public}d",
               nextBytes, (int)preserved);
    }
    return kVTVideoEncoderNotAvailableNowErr;
}
