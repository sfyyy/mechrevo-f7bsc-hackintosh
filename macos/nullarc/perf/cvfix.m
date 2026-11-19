// cvfix [secs] (VBL_CONST=ns): cvjitter, but with CoreVideo's private pointer to SLSDisplayGetCurrentVBLDelta replaced
// by a function returning a constant, i.e. what IOFramebuffer publishes in its timing mode (vblDelta fixed from the
// mode's detailed timing). Tests whether a constant VBL delta makes CVDisplayLink sane, without touching the kernel.
// Original cvjitter: how sane CVDisplayLink's timestamps are. Chrome takes its vsync timebase from out->hostTime and its
// interval from videoRefreshPeriod, so both must track the real vblank. Reports the spread of the reported period, how
// far out->hostTime is from (now->hostTime + 1 period) i.e. the next vblank, and callback gaps. Also the kernel tick
// jitter (debug.phxfb_vbl_jitter_us_max, after the f7bsc vbl-spin patch).
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
static uint64_t gConst; static uint64_t (*gReal)(uint32_t);
static uint64_t constDelta(uint32_t d) { return gConst; }
static void **findCVSym(const char *name) {   // CoreVideo's in-memory symtab (shared cache keeps it)
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if (!strstr(_dyld_get_image_name(i), "/CoreVideo.framework/")) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i); intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const struct load_command *lc = (const void *)(mh + 1); const struct symtab_command *st = 0; const struct segment_command_64 *le = 0;
        for (uint32_t c = 0; c < mh->ncmds; c++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT")) le = (const void *)lc; }
        const char *lb = (const char *)(le->vmaddr + slide - le->fileoff);
        const struct nlist_64 *sy = (const void *)(lb + st->symoff); const char *str = lb + st->stroff;
        for (uint32_t s = 0; s < st->nsyms; s++) if (!strcmp(str + sy[s].n_un.n_strx, name)) return (void **)(sy[s].n_value + slide);
    }
    return NULL;
}
#define N 20000
static double per[N], err[N], gap[N]; static int n; static uint64_t last; static mach_timebase_info_data_t tb;
static double ms(int64_t d) { return (double)d * tb.numer / tb.denom / 1e6; }
static CVReturn cb(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags f, CVOptionFlags *fo, void *c) {
    uint64_t t = mach_absolute_time(); if (n >= N) return 0;
    const double p = out->videoTimeScale ? 1000.0 * out->videoRefreshPeriod / out->videoTimeScale : 0;
    per[n] = p; err[n] = ms((int64_t)out->hostTime - (int64_t)now->hostTime) - p; gap[n] = last ? ms(t - last) : 0; n++; last = t; return 0; }
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int k, double q) { qsort(v, k, sizeof *v, cmp); return v[(int)(q * (k - 1))]; }
int main(int argc, char **argv) {
    mach_timebase_info(&tb); int secs = argc > 1 ? atoi(argv[1]) : 15;
    int64_t jm = 0; size_t sl = sizeof jm; int has = sysctlbyname("debug.phxfb_vbl_jitter_us_max", &jm, &sl, NULL, 0) == 0;
    CVDisplayLinkRef l; CVDisplayLinkCreateWithActiveCGDisplays(&l); CVDisplayLinkSetOutputCallback(l, cb, 0);
    const char *e = getenv("VBL_CONST"); gConst = e ? strtoull(e, 0, 0) : 0;
    if (gConst) { void **p = findCVSym("__ZL30cvSLSDisplayGetCurrentVBLDelta");
        if (!p || !*p) { fprintf(stderr, "pointer not found/loaded\n"); return 1; }
        uintptr_t pg = (uintptr_t)p & ~(uintptr_t)(getpagesize() - 1);
        if (mprotect((void *)pg, getpagesize(), PROT_READ | PROT_WRITE)) { perror("mprotect"); return 1; }
        gReal = *p; *p = (void *)constDelta; fprintf(stderr, "CV VBL delta forced to %llu (real now %llu)\n", gConst, gReal(CGMainDisplayID())); }
    CVDisplayLinkStart(l);
    sleep(secs); CVDisplayLinkStop(l);
    double ae[N]; int h[6] = {0}; double pmin = 1e9, pmax = 0;
    for (int i = 0; i < n; i++) { ae[i] = fabs(err[i]); if (per[i] < pmin) pmin = per[i]; if (per[i] > pmax) pmax = per[i];
        if (i) { int k = (int)(gap[i] / 16.67 + 0.5); k = k < 1 ? 1 : k > 6 ? 6 : k; h[k - 1]++; } }
    double gmax = 0; for (int i = 1; i < n; i++) if (gap[i] > gmax) gmax = gap[i];
    printf("CVJITTER %d s, %d callbacks: period %.3f..%.3f ms; |out.host - next vblank| median %.2f p90 %.2f max %.1f ms; "
           "gaps 1/2/3/4/5/6+: %d/%d/%d/%d/%d/%d worst %.1f ms", secs, n, pmin, pmax, pct(ae, n, .5), pct(ae, n, .9), pct(ae, n, 1), h[0], h[1], h[2], h[3], h[4], h[5], gmax);
    if (has) { int64_t j = 0; sl = sizeof j; sysctlbyname("debug.phxfb_vbl_jitter_us_max", &j, &sl, NULL, 0); printf("; kernel tick jitter max %lld us", (long long)j); }
    printf("\n");
}
