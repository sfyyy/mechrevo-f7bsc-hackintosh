#ifndef PHOENIX_VIDEO_CONTEXT_H
#define PHOENIX_VIDEO_CONTEXT_H

#include <IOKit/IOKitLib.h>

typedef struct __IOAccelDevice *Device;
typedef struct __IOAccelShared *Shared;
typedef struct __IOAccelVideoContext *Video;
extern Device IOAccelDeviceCreate(io_service_t accelerator);
extern Shared IOAccelSharedCreate(Device device);
extern Video IOAccelVideoContextCreate(Shared shared);
extern io_connect_t IOAccelVideoContextGetConnect(Video video);
extern void IOAccelVideoContextRelease(Video video);
extern void IOAccelSharedRelease(Shared shared);
extern void IOAccelDeviceRelease(Device device);

typedef struct {
    Device device;
    Shared shared;
    Video video;
} PhoenixVideoContext;

// The connection returned by GetConnect is borrowed from the video context.
static inline io_connect_t PhoenixOpenVideoContext(PhoenixVideoContext *ctx, io_service_t service) {
    ctx->device = IOAccelDeviceCreate(service);
    if (ctx->device) ctx->shared = IOAccelSharedCreate(ctx->device);
    if (ctx->shared) ctx->video = IOAccelVideoContextCreate(ctx->shared);
    return ctx->video ? IOAccelVideoContextGetConnect(ctx->video) : IO_OBJECT_NULL;
}

static inline void PhoenixReleaseVideoContext(PhoenixVideoContext *ctx) {
    if (ctx->video) IOAccelVideoContextRelease(ctx->video);
    if (ctx->shared) IOAccelSharedRelease(ctx->shared);
    if (ctx->device) IOAccelDeviceRelease(ctx->device);
}

#endif
