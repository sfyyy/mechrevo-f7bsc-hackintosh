// cvts2: raw CVDisplayLink timestamps next to WindowServer's published VBL and the kernel tick count, per callback.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <CoreGraphics/CoreGraphics.h>
extern uint64_t SLSDisplayGetPriorVBLTime(CGDirectDisplayID);
extern uint64_t SLSDisplayGetCurrentVBLDelta(CGDirectDisplayID);
static int n; static uint64_t last;
static int64_t rd(const char *s) { int64_t v = 0; size_t l = sizeof v; sysctlbyname(s, &v, &l, NULL, 0); return v; }
static CVReturn cb(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags f, CVOptionFlags *fo, void *c) {
    uint64_t t = mach_absolute_time(); uint64_t pv = SLSDisplayGetPriorVBLTime(CGMainDisplayID());
    if (n++ < 60) printf("gap %6.2f | now.host %+7.2f out.host %+7.2f | WS prior %+7.2f delta %.3f | ktick %lld\n",
        last ? (t-last)/1e6 : 0., ((double)now->hostTime-(double)t)/1e6, ((double)out->hostTime-(double)t)/1e6, ((double)pv-(double)t)/1e6,
        SLSDisplayGetCurrentVBLDelta(CGMainDisplayID())/1e6, rd("debug.phxfb_vbl_n"));
    last = t; return 0; }
int main() { CVDisplayLinkRef l; CVDisplayLinkCreateWithActiveCGDisplays(&l); CVDisplayLinkSetOutputCallback(l, cb, 0); CVDisplayLinkStart(l); sleep(2); }
