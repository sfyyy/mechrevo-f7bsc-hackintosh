// gpuping secs: tiny Metal command buffer every ~4 ms, prints completion latency >20 ms with time since T0 (CLOCK as stall)
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <sys/time.h>
int main(int argc, char **argv) { @autoreleasepool {
    int secs = argc > 1 ? atoi(argv[1]) : 30; double T0arg = argc > 2 ? atof(argv[2]) : 0;
    setvbuf(stdout, 0, _IOLBF, 0);
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> b = [d newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    struct timeval tv; gettimeofday(&tv, 0); double now0 = tv.tv_sec + tv.tv_usec / 1e6, off = T0arg ? now0 - T0arg : 0;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); double tk = (double)tb.numer / tb.denom / 1e6;
    uint64_t t0 = mach_absolute_time(); long n = 0, h[5] = {0}; double worst = 0;
    while ((mach_absolute_time() - t0) * tk < secs * 1e3) { @autoreleasepool {
        uint64_t a = mach_absolute_time();
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e fillBuffer:b range:NSMakeRange(0, 4096) value:(uint8_t)n]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        double l = (mach_absolute_time() - a) * tk; n++; if (l > worst) worst = l;
        h[l < 2 ? 0 : l < 8 ? 1 : l < 20 ? 2 : l < 50 ? 3 : 4]++;
        if (l > 20) printf("%9.3f GPU latency %.1f ms\n", off + (a - t0) * tk / 1e3, l);
        usleep(4000); } }
    printf("GPUPING %ld cbs; <2/<8/<20/<50/>=50 ms: %ld/%ld/%ld/%ld/%ld worst %.1f\n", n, h[0], h[1], h[2], h[3], h[4], worst);
}}
