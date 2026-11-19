// vblprobe [secs]: CVDisplayLink on the main display; histogram of callback intervals in refresh periods and the
// longest gap. Independent of Chrome and of the Metal plugin: a gap here is a stall in the VBL source / display pipe.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <CoreGraphics/CoreGraphics.h>
extern uint64_t SLSDisplayGetPriorVBLTime(CGDirectDisplayID);     // what CVDisplayLink re-syncs to (WindowServer shmem)
extern uint64_t SLSDisplayGetCurrentVBLDelta(CGDirectDisplayID);
static uint64_t ringT[8], ringP[8], ringD[8], ringO[8]; static unsigned ri;
static uint64_t last, h[6], n; static int64_t lastVideo; static uint64_t lastOutHost; static double worst, period; static mach_timebase_info_data_t tb;
static CVReturn cb(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags f, CVOptionFlags *fo, void *ctx)
{
    uint64_t t = mach_absolute_time();
    if (getenv("VBL_GAPS")) { const unsigned j = ri++ & 7; ringT[j] = t; ringO[j] = out->hostTime;
        ringP[j] = SLSDisplayGetPriorVBLTime(CGMainDisplayID()); ringD[j] = SLSDisplayGetCurrentVBLDelta(CGMainDisplayID()); }
    if (last) { double ms = (double)(t - last) * tb.numer / tb.denom / 1e6; if (ms > worst) worst = ms;
        int k = (int)(ms / period + 0.5); if (k < 1) k = 1; if (k > 6) k = 6; h[k - 1]++; n++;
        if (k > 1 && getenv("VBL_GAPS")) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            const double dv = out->videoRefreshPeriod ? (double)(out->videoTime - lastVideo) / out->videoRefreshPeriod : 0;
            const double late = ((double)t - (double)out->hostTime) * tb.numer / tb.denom / 1e6;
            const double dout = (double)(out->hostTime - lastOutHost) * tb.numer / tb.denom / 1e6;
            fprintf(stderr, "gap %.1f ms at %ld.%03ld: output time advanced %.2f periods (%.1f ms host), callback %.1f ms %s its output time, rate %.4f flags %llx\n",
                    ms, (long)ts.tv_sec, ts.tv_nsec / 1000000, dv, dout, late < 0 ? -late : late, late < 0 ? "before" : "after", out->rateScalar, (unsigned long long)out->flags);
            const double K = (double)tb.numer / tb.denom / 1e6;
            for (unsigned q = ri - 8; q != ri; q++) { const unsigned j = q & 7;
                fprintf(stderr, "    cb at %+8.1f ms: WS prior VBL %+8.1f ms, delta %.3f ms, CV output time %+.1f ms\n", ((double)ringT[j] - (double)t) * K,
                        ((double)ringP[j] - (double)t) * K, ringD[j] * K, ((double)ringO[j] - (double)t) * K); } } }
    lastVideo = out->videoTime; lastOutHost = out->hostTime;
    last = t; return kCVReturnSuccess;
}
int main(int argc, char **argv)
{
    mach_timebase_info(&tb); int secs = argc > 1 ? atoi(argv[1]) : 25;
    CVDisplayLinkRef l; CVDisplayLinkCreateWithActiveCGDisplays(&l);
    CVTime p = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(l); period = p.timeValue ? 1000.0 * p.timeValue / p.timeScale : 16.67;
    CVDisplayLinkSetOutputCallback(l, cb, NULL); CVDisplayLinkStart(l);
    sleep(secs); CVDisplayLinkStop(l);
    printf("VBL period %.2f ms, %llu callbacks, intervals in periods 1/2/3/4/5/6+: %llu/%llu/%llu/%llu/%llu/%llu, worst %.1f ms\n",
           period, n, h[0], h[1], h[2], h[3], h[4], h[5], worst);
}
