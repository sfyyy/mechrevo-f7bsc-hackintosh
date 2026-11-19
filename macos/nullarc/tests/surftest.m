// surftest: exercises NVMTLDriver's IOSurface texture path the way CoreAnimation does (a new MTLTexture per frame over
// pooled IOSurfaces), checks every frame's pixels, and reports the plugin's resource use.
// Build: clang -fobjc-arc -framework Foundation -framework Metal -framework IOSurface surftest.m -o /tmp/surftest
// Run: /tmp/surftest [frames] [width] (default 600 200: rows of 832 bytes, a multiple of 32 but not of 256 -> the
// private-image + surface-in path). PASS = every frame's pixels match. With NVMTL_LOG_PATH=/tmp/surftest.log the plugin's
// "image census" / "surface-in census" lines show the resources stay bounded. Apps load a newly installed plugin at
// once, WindowServer only after a reboot: run this (and perf/bench2.sh) BEFORE rebooting into a new plugin.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <mach/mach.h>
static IOSurfaceRef mk(int w, int h) {
    NSDictionary *p = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
                         (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}
static void fill(IOSurfaceRef s, uint32_t v) {
    IOSurfaceLock(s, 0, NULL);
    uint8_t *b = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s), w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s);
    for (size_t y = 0; y < h; y++) { uint32_t *r = (uint32_t *)(b + y * row); for (size_t x = 0; x < w; x++) r[x] = v ^ (uint32_t)(x * 7 + y * 131); }
    IOSurfaceUnlock(s, 0, NULL);
}
static uint64_t footprint(void) {
    task_vm_info_data_t i; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &n) == KERN_SUCCESS ? i.phys_footprint : 0;
}
// one frame: wrap the surface in a fresh texture, copy it into a buffer in a command buffer, check the pixels
static int frame(id<MTLDevice> d, id<MTLCommandQueue> q, IOSurfaceRef s, uint32_t v, id<MTLBuffer> out, int earlierCB) {
    int w = (int)IOSurfaceGetWidth(s), h = (int)IOSurfaceGetHeight(s);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
    if (!t) { printf("texture nil\n"); return -1; }
    id<MTLCommandBuffer> pre = earlierCB ? [q commandBuffer] : nil;   // created first, committed after the encode below
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [e endEncoding];
    if (pre) { [pre commit]; }
    [cb commit]; [cb waitUntilCompleted];
    const uint32_t *p = out.contents; int bad = 0;
    for (int y = 0; y < h && bad < 1; y++) for (int x = 0; x < w; x++)
        if (p[y * w + x] != (v ^ (uint32_t)(x * 7 + y * 131))) { printf("MISMATCH value %08x at %d,%d: got %08x\n", v, x, y, p[y * w + x]); bad = 1; break; }
    return bad;
}
int main(int argc, char **argv) { @autoreleasepool {
    int frames = argc > 1 ? atoi(argv[1]) : 600, W = argc > 2 ? atoi(argv[2]) : 200, H = 120;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    printf("device %s\n", d.name.UTF8String);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> out = [d newBufferWithLength:(NSUInteger)W * H * 4 options:MTLResourceStorageModeShared];
    IOSurfaceRef pool[4]; for (int i = 0; i < 4; i++) pool[i] = mk(W, H);
    printf("surface %dx%d rows %zu bytes\n", W, H, IOSurfaceGetBytesPerRow(pool[0]));
    int bad = 0; uint64_t f0 = footprint(), t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // 1. pooled: four surfaces in turn, new contents each frame, every other frame with an earlier command buffer
    for (int i = 0; i < frames; i++) @autoreleasepool { IOSurfaceRef s = pool[i & 3]; uint32_t v = 0x10000u * (uint32_t)i + 0x55;
        fill(s, v); bad += frame(d, q, s, v, out, i & 1); }
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    printf("pooled: %d frames, %d bad, %.3f ms/frame, footprint %+.1f MB\n", frames, bad, (t1 - t0) / 1e6 / frames, ((double)footprint() - f0) / 1048576);
    // 2. churn: a new surface every frame, released at once (the keep cache must stay bounded)
    uint64_t f1 = footprint(); int bad2 = 0;
    for (int i = 0; i < frames; i++) @autoreleasepool { IOSurfaceRef s = mk(W, H); uint32_t v = 0x77000000u + (uint32_t)i;
        fill(s, v); bad2 += frame(d, q, s, v, out, 0); CFRelease(s); }
    printf("churn: %d frames, %d bad, footprint %+.1f MB (vs after pooled)\n", frames, bad2, ((double)footprint() - f1) / 1048576);
    // 3. after a pause the idle entries age out: one more frame triggers the sweep
    usleep(700 * 1000);
    @autoreleasepool { fill(pool[0], 0xabcdef); bad += frame(d, q, pool[0], 0xabcdef, out, 0); }
    printf("RESULT %s\n", bad + bad2 ? "FAIL" : "PASS");
    return bad + bad2 ? 1 : 0;
} }
