// vbltick [secs]: polls PhxAMDFB's software-VBL counters (debug.phxfb_vbl_n / _late_us_sum) every 0.25 ms and reports,
// per tick, how late the kernel timer fired (late_us_sum delta) and how many ticks were missed (gap in periods).
// No root needed. Tells a VBL-source stall (the tick itself late or skipped) from anything above it.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
static int64_t rd(const char *n) { int64_t v = 0; size_t l = sizeof v; sysctlbyname(n, &v, &l, NULL, 0); return v; }
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 25;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    int64_t n0 = rd("debug.phxfb_vbl_n"), s0 = rd("debug.phxfb_vbl_late_us_sum"), n = n0, s = s0;
    uint64_t t0 = mach_absolute_time(), last = t0, end = t0 + (uint64_t)secs * 1000000000ull * tb.denom / tb.numer;
    uint64_t hl[6] = {0}, hg[6] = {0}; int64_t worst_late = 0; double worst_gap = 0;
    while (mach_absolute_time() < end) {
        usleep(250);
        int64_t n1 = rd("debug.phxfb_vbl_n");
        if (n1 == n) continue;
        int64_t s1 = rd("debug.phxfb_vbl_late_us_sum");
        uint64_t t = mach_absolute_time(); double gap = (double)(t - last) * tb.numer / tb.denom / 1e6;
        int64_t late = (s1 - s) / (n1 - n);
        if (late > worst_late) worst_late = late;
        if (gap > worst_gap) worst_gap = gap;
        int kl = late < 500 ? 0 : late < 2000 ? 1 : late < 8000 ? 2 : late < 17000 ? 3 : late < 34000 ? 4 : 5; hl[kl]++;
        int kg = (int)(gap / 16.67 + 0.5); if (kg < 1) kg = 1; if (kg > 6) kg = 6; hg[kg - 1]++;
        if (late > 8000 || gap > 30) printf("  t=%.3f s tick gap %.1f ms, late %lld us (%lld ticks)\n", (double)(t - t0) * tb.numer / tb.denom / 1e9, gap, (long long)late, (long long)(n1 - n));
        n = n1; s = s1; last = t;
    }
    printf("VBLTICK %lld ticks in %d s; lateness <0.5/<2/<8/<17/<34/>=34 ms: %llu/%llu/%llu/%llu/%llu/%llu, worst %lld us; "
           "userspace-seen gaps in periods 1/2/3/4/5/6+: %llu/%llu/%llu/%llu/%llu/%llu, worst %.1f ms\n",
           (long long)(n - n0), secs, hl[0], hl[1], hl[2], hl[3], hl[4], hl[5], (long long)worst_late, hg[0], hg[1], hg[2], hg[3], hg[4], hg[5], worst_gap);
}
