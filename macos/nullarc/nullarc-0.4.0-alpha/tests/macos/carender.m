// carender: runs CoreAnimation's own Metal renderer (CARenderer) in-process over a WindowServer-like layer tree:
// N IOSurface-backed layers, each rotating through K surfaces every frame (Chrome's swap chain), rounded corners,
// opacity, moving positions. Every frame is rendered into an MTLTexture, read back and checked at each layer's
// centre against the colour of the surface it shows. This is the CA draw pattern WindowServer feeds the plugin,
// without putting an untested plugin in WindowServer.
//   usage: carender [frames=600] [layers=40] [surfaces-per-layer=4] [check-every=1, 0 = no readback]
// Exit 0 = every checked pixel right. CARENDER_ALPHA=1 makes every third layer 60% opaque (blending; those are not
// checked); CARENDER_IMAGES=N gives every Nth layer a NEW CGImage each frame instead of a surface (CA uploads it with
// newTiledTextureWithBytesNoCopy; odd row pitches take the plugin's buffer-texture stand-in path); CARENDER_DUMP=N writes frame N as raw BGRA to /tmp/carender.bgra, to compare two plugin builds byte for byte.
// Run against an uninstalled build with runp.sh (DYLD shim), never by installing it first.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <time.h>

#define W 1280
#define H 800

static uint32_t imgcol(int i, int f)
{
    uint32_t h = (uint32_t)(i * 2654435761u) ^ (uint32_t)(f * 40503u); h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return 0xff000000u | ((40 + (h & 255) % 200) << 16) | ((40 + ((h >> 8) & 255) % 200) << 8) | (40 + ((h >> 16) & 255) % 200);
}
static CGImageRef mkimage(int w, int h, uint32_t bgra, CGColorSpaceRef cs)
{
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    uint8_t *b = CGBitmapContextGetData(c); size_t rb = CGBitmapContextGetBytesPerRow(c);
    for (int y = 0; y < h; y++) { uint32_t *r = (uint32_t *)(b + y * rb); for (int x = 0; x < w; x++) r[x] = bgra; }
    CGImageRef im = CGBitmapContextCreateImage(c); CGContextRelease(c); return im;
}

static IOSurfaceRef mksurf(int w, int h, uint32_t bgra)
{
    NSDictionary *p = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
                         (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    IOSurfaceSetValue(s, kIOSurfaceColorSpace, (__bridge CFStringRef)@"sRGB");
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFPropertyListRef pl = CGColorSpaceCopyPropertyList(srgb);
    if (pl) { IOSurfaceSetValue(s, kIOSurfaceColorSpace, pl); CFRelease(pl); }
    CGColorSpaceRelease(srgb);
    IOSurfaceLock(s, 0, NULL);
    uint8_t *b = IOSurfaceGetBaseAddress(s); size_t rb = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++) { uint32_t *r = (uint32_t *)(b + y * rb); for (int x = 0; x < w; x++) r[x] = bgra; }
    IOSurfaceUnlock(s, 0, NULL);
    return s;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int frames = argc > 1 ? atoi(argv[1]) : 600, nl = argc > 2 ? atoi(argv[2]) : 40, ns = argc > 3 ? atoi(argv[3]) : 4;
        int every = argc > 4 ? atoi(argv[4]) : 1;
        const int nimg = getenv("CARENDER_IMAGES") ? atoi(getenv("CARENDER_IMAGES")) : 0;
        uint64_t worst_ns = 0;
        if (nl > 256) nl = 256;
        if (ns > 8) ns = 8;
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) { puts("no device"); return 2; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [d newTextureWithDescriptor:td];
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        id<MTLCommandQueue> q = [d newCommandQueue];
        CARenderer *r = [CARenderer rendererWithMTLTexture:target options:@{ kCARendererColorSpace: (__bridge id)cs, kCARendererMetalCommandQueue: q }];
        if (!r) { puts("no CARenderer"); return 3; }
        CALayer *root = [CALayer layer];
        root.frame = CGRectMake(0, 0, W, H); root.backgroundColor = CGColorCreateGenericRGB(0, 0, 0, 1);
        [CATransaction begin];
        r.layer = root; r.bounds = root.frame;
        CALayer *L[256]; IOSurfaceRef S[256][8]; uint32_t C[256][8];
        const int lw[4] = { 61, 70, 37, 50 }, lh[4] = { 40, 64, 33, 90 };   // odd row pitches: CA's private-image path
        for (int i = 0; i < nl; i++) {
            L[i] = [CALayer layer];
            const int w = lw[i & 3], h = lh[i & 3];
            L[i].bounds = CGRectMake(0, 0, w, h);
            L[i].cornerRadius = (i % 3) ? 6 : 0; L[i].masksToBounds = (i % 3) != 0;
            L[i].contentsGravity = kCAGravityResize;
            for (int k = 0; k < ns; k++) {
                // distinct, saturated colours so a wrong surface is never mistaken for the right one
                uint8_t rr = (uint8_t)(40 + (i * 37) % 200), gg = (uint8_t)(40 + (k * 61 + i * 13) % 200), bb = (uint8_t)(40 + ((i + k) * 89) % 200);
                C[i][k] = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | bb;
                S[i][k] = mksurf(w, h, C[i][k]);
            }
            [root addSublayer:L[i]];
        }
        [CATransaction commit]; [CATransaction flush];
        id<MTLBuffer> out = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        int bad = 0, checked = 0;
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), render_ns = 0;
        const int cols = 16;
        for (int f = 0; f < frames; f++) @autoreleasepool {
            [CATransaction begin]; [CATransaction setDisableActions:YES];
            for (int i = 0; i < nl; i++) {
                // a grid that shifts a little each frame; layers never overlap so every centre is that layer's colour
                const int cx = 40 + (i % cols) * 78 + (f % 7), cy = 50 + (i / cols) * 110 + (f % 5);
                L[i].position = CGPointMake(cx, cy);
                if (nimg > 0 && i % nimg == 0) {
                    const int w = lw[i & 3], h = lh[i & 3];
                    CGImageRef im = mkimage(w, h, imgcol(i, f), cs); L[i].contents = (__bridge id)im; CGImageRelease(im);
                } else L[i].contents = (__bridge id)S[i][(f + i) % ns];
                L[i].opacity = getenv("CARENDER_ALPHA") && (i % 3) == 0 ? 0.6f : 1.0f;   // translucent: blended over the background
            }
            [CATransaction commit]; [CATransaction flush];
            uint64_t a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
            [r addUpdateRect:r.bounds];
            if (f == 0) { CGRect ub = [r updateBounds]; printf("updateBounds %.0f,%.0f %.0fx%.0f\n", ub.origin.x, ub.origin.y, ub.size.width, ub.size.height); }
            [r render];
            [r endFrame];
            { const uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - a; render_ns += dt; if (f && dt > worst_ns) worst_ns = dt; }
            if (every <= 0 || (f % every)) continue;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                       toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { printf("frame %d readback status %lu\n", f, (unsigned long)cb.status); bad++; continue; }
            const uint32_t *px = out.contents;
            if (getenv("CARENDER_DUMP") && f == atoi(getenv("CARENDER_DUMP"))) {   // raw BGRA W x H for inspection
                FILE *df = fopen("/tmp/carender.bgra", "wb"); if (df) { fwrite(px, 4, W * H, df); fclose(df); }
            }
            for (int i = 0; i < nl; i++) {
                const int cx = 40 + (i % cols) * 78 + (f % 7), cy = 50 + (i / cols) * 110 + (f % 5);
                if (cx >= W || cy >= H || (getenv("CARENDER_ALPHA") && (i % 3) == 0)) continue;   // translucent: compare dumps instead
                const uint32_t got = px[cy * W + cx], want = nimg > 0 && i % nimg == 0 ? imgcol(i, f) : C[i][(f + i) % ns];
                int ok = 1;
                for (int s = 0; s < 24; s += 8) { int dg = (int)((got >> s) & 255) - (int)((want >> s) & 255); if (dg < -3 || dg > 3) ok = 0; }
                checked++;
                if (!ok && bad++ < 6) printf("frame %d layer %d at %d,%d: got %08x want %08x\n", f, i, cx, cy, got, want);
            }
        }
        uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        printf("RESULT %s %d frames x %d layers x %d surfaces (images every %d): %.3f ms/frame wall, %.3f ms/frame in CARenderer (worst %.1f), %d pixels checked, %d wrong\n",
               bad ? "FAIL" : "PASS", frames, nl, ns, nimg, (double)(t1 - t0) / 1e6 / frames, (double)render_ns / 1e6 / frames, (double)worst_ns / 1e6, checked, bad);
        return bad ? 1 : 0;
    }
}
