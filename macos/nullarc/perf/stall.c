// stall [secs]: finds periodic stutters. Two busy threads report their own stalls >5 ms (system-wide when both stall at once);
// main thread polls PhxAMDFB: vbl tick lateness and dbuf flips (WindowServer presents). All events printed with time.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
static double tk; static uint64_t T0; static volatile int stop;
static double ms(uint64_t t) { return (double)t * tk / 1e6; }
static int64_t rd(const char *n) { int64_t v = 0; size_t l = sizeof v; sysctlbyname(n, &v, &l, NULL, 0); return v; }
static void *spin(void *a) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint64_t last = mach_absolute_time();
    while (!stop) { uint64_t t = mach_absolute_time(); if (ms(t - last) > 5) printf("%9.3f SPIN%ld stall %.1f ms\n", ms(last - T0) / 1e3, (long)a, ms(t - last)); last = t; }
    return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IOLBF, 0);
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); tk = (double)tb.numer / tb.denom;
    T0 = mach_absolute_time(); { struct timeval tv; gettimeofday(&tv, 0); printf("T0 %ld.%06d\n", (long)tv.tv_sec, (int)tv.tv_usec); }
    pthread_t th[2]; for (long i = 0; i < 2; i++) pthread_create(&th[i], 0, spin, (void *)i);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    int64_t n = rd("debug.phxfb_vbl_n"), s = rd("debug.phxfb_vbl_late_us_sum"), f = rd("debug.phxfb_dbuf_flips");
    uint64_t lastflip = mach_absolute_time(), lasttick = lastflip; long flips = 0, ticks = 0, fg[6] = {0};
    while (ms(mach_absolute_time() - T0) < secs * 1e3) {
        usleep(250);
        uint64_t t = mach_absolute_time();
        int64_t n1 = rd("debug.phxfb_vbl_n");
        if (n1 != n) { int64_t s1 = rd("debug.phxfb_vbl_late_us_sum"); int64_t late = (s1 - s) / (n1 - n);
            if (late > 3000 || n1 - n > 1) printf("%9.3f TICK late %lld us, %lld ticks, gap %.1f ms\n", ms(t - T0) / 1e3, (long long)late, (long long)(n1 - n), ms(t - lasttick));
            ticks += n1 - n; n = n1; s = s1; lasttick = t; }
        int64_t f1 = rd("debug.phxfb_dbuf_flips");
        if (f1 != f) { double g = ms(t - lastflip); int k = (int)(g / 16.667 + 0.5);
            if (k >= 1 && k <= 6) fg[k - 1]++; else if (k > 6) fg[5]++;
            if (g > 25 && g < 300) printf("%9.3f FLIP gap %.1f ms (%d periods)\n", ms(t - T0) / 1e3, g, k);
            flips += f1 - f; f = f1; lastflip = t; }
    }
    stop = 1; for (int i = 0; i < 2; i++) pthread_join(th[i], 0);
    printf("SUMMARY %d s: ticks %ld, flips %ld; flip gaps (periods, <300ms) 1/2/3/4/5/6+: %ld/%ld/%ld/%ld/%ld/%ld\n", secs, ticks, flips, fg[0], fg[1], fg[2], fg[3], fg[4], fg[5]);
}
