// vblstale [secs]: polls, every 0.25 ms, the VBL time WindowServer publishes to clients (CVDisplayLinkGetCurrentTime
// reads it from WindowServer's realtime display shmem) and PhxAMDFB's kernel tick counter (debug.phxfb_vbl_n).
// Reports every stretch where WindowServer's VBL time stood still > 1.5 periods while the kernel kept ticking:
// that is WindowServer not consuming its VBL events, and every app's display link (Chrome's rAF clock) skips with it.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <CoreGraphics/CoreGraphics.h>
extern uint64_t SLSDisplayGetPriorVBLTime(CGDirectDisplayID);   // SkyLight: the last VBL WindowServer recorded (host time)
static CVReturn nop(CVDisplayLinkRef l, const CVTimeStamp *a, const CVTimeStamp *b, CVOptionFlags f, CVOptionFlags *fo, void *c) { return kCVReturnSuccess; }
static int64_t rd(const char *n) { int64_t v = 0; size_t l = sizeof v; sysctlbyname(n, &v, &l, NULL, 0); return v; }
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 25;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    CVDisplayLinkRef l; CVDisplayLinkCreateWithActiveCGDisplays(&l); CVDisplayLinkSetOutputCallback(l, nop, NULL); CVDisplayLinkStart(l); usleep(100000);
    CVTime p = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(l); double period = p.timeValue ? 1000.0 * p.timeValue / p.timeScale : 16.67;
    const double ms_per = (double)tb.numer / tb.denom / 1e6;
    uint64_t t0 = mach_absolute_time(), end = t0 + (uint64_t)(secs * 1000.0 / ms_per);
    uint64_t lastHost = 0, since = t0; int64_t kn0 = rd("debug.phxfb_vbl_n"), knSince = kn0;
    uint64_t h[6] = {0}; double worst = 0; int stalls = 0;
    while (mach_absolute_time() < end) {
        usleep(250);
        const uint64_t vbl = SLSDisplayGetPriorVBLTime(CGMainDisplayID());
        uint64_t now = mach_absolute_time();
        if (vbl != lastHost) {
            if (lastHost) {
                double stood = (now - since) * ms_per; int64_t kn = rd("debug.phxfb_vbl_n");
                int k = (int)(stood / period + 0.5); if (k < 1) k = 1; if (k > 6) k = 6; h[k - 1]++;
                if (stood > worst) worst = stood;
                if (stood > 1.5 * period) { stalls++; struct timespec r; clock_gettime(CLOCK_REALTIME, &r);
                    printf("  WS VBL time stood %.1f ms (kernel ticked %lld times meanwhile) at %ld.%03ld\n", stood, (long long)(kn - knSince), (long)r.tv_sec, r.tv_nsec / 1000000); }
                knSince = kn;
            }
            lastHost = vbl; since = now;
        }
    }
    printf("VBLSTALE %d s: WindowServer VBL-time updates, held for 1/2/3/4/5/6+ periods: %llu/%llu/%llu/%llu/%llu/%llu, worst %.1f ms, %d stalls; kernel ticks %lld\n",
           secs, h[0], h[1], h[2], h[3], h[4], h[5], worst, stalls, (long long)(rd("debug.phxfb_vbl_n") - kn0));
}
