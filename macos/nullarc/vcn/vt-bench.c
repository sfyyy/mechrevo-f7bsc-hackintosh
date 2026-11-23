/* Decode one Annex B H.264 file through VideoToolbox, forced hardware or forced software,
 * and report wall time, fps and CPU per frame (this process + VTDecoderXPCService).
 *
 * xcrun clang -std=c11 -O2 -Wall -Wextra vt-bench.c -framework VideoToolbox -framework CoreMedia \
 *   -framework CoreVideo -framework CoreFoundation -o vt-bench
 * ./vt-bench --hardware|--software stream.h264 [loops] [--realtime fps]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct { const uint8_t *p; size_t n; } Nal;
typedef struct { uint8_t *data; size_t size; int idr; } AU;

static atomic_int frames, errors;
static uint64_t sumLatency, maxLatency;
static double idrLat[64]; static int idrN;

static double now(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static unsigned firstMbIsZero(const uint8_t *p, size_t n) { return n > 1 && (p[1] & 0x80); } /* ue(v)==0 */

static size_t splitNals(const uint8_t *b, size_t n, Nal *out, size_t cap) {
    size_t k = 0, i = 0, start = SIZE_MAX;
    while (i + 2 < n) {
        if (!b[i] && !b[i + 1] && b[i + 2] == 1) {
            if (start != SIZE_MAX && k < cap) {
                size_t end = i;
                while (end > start && !b[end - 1]) --end;
                out[k++] = (Nal){b + start, end - start};
            }
            i += 3; start = i;
        } else ++i;
    }
    if (start != SIZE_MAX && start < n && k < cap) out[k++] = (Nal){b + start, n - start};
    return k;
}

static double serviceCpu(void) {
    pid_t pids[4096];
    int n = proc_listallpids(pids, sizeof(pids)) ;
    double total = 0;
    for (int i = 0; i < n; ++i) {
        char name[64] = {0};
        if (proc_name(pids[i], name, sizeof(name)) <= 0 || strcmp(name, "VTDecoderXPCService")) continue;
        struct rusage_info_v2 ri;
        if (proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t *)&ri) == 0) {
            static mach_timebase_info_data_t tb;
            if (!tb.denom) mach_timebase_info(&tb);
            total += (double)(ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom / 1e9;
        }
    }
    return total;
}

static double selfCpu(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

static void callback(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime d) {
    (void)ref; (void)f; (void)pts; (void)d;
    if (st || !img) { atomic_fetch_add(&errors, 1); return; }
    double lat = now() - ((double *)src)[0];
    if (((double *)src)[1] == 0 && idrN < 64) idrLat[idrN++] = lat * 1000;
    uint64_t us = (uint64_t)(lat * 1e6);
    sumLatency += us; if (us > maxLatency) maxLatency = us;
    atomic_fetch_add(&frames, 1);
    free(src);
}

int main(int argc, char **argv) {
    if (argc < 3 || (strcmp(argv[1], "--hardware") && strcmp(argv[1], "--software"))) {
        fprintf(stderr, "usage: %s --hardware|--software stream.h264 [loops] [--realtime fps]\n", argv[0]);
        return 2;
    }
    const int hw = !strcmp(argv[1], "--hardware");
    int loops = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 1;
    double pace = 0;
    for (int i = 3; i + 1 < argc; ++i) if (!strcmp(argv[i], "--realtime")) pace = 1.0 / atof(argv[i + 1]);
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != n) return 1;
    fclose(f);

    static Nal nals[200000];
    size_t count = splitNals(buf, n, nals, 200000);
    const uint8_t *sps = NULL, *pps = NULL; size_t spsN = 0, ppsN = 0;
    AU *aus = calloc(count + 1, sizeof(AU)); size_t nau = 0;
    for (size_t i = 0; i < count; ++i) {
        int t = nals[i].p[0] & 31;
        if (t == 7 && !sps) { sps = nals[i].p; spsN = nals[i].n; }
        else if (t == 8 && !pps) { pps = nals[i].p; ppsN = nals[i].n; }
        else if (t == 1 || t == 5) {
            if (firstMbIsZero(nals[i].p, nals[i].n) || !nau) { aus[nau].idr = t == 5; ++nau; }
            AU *a = &aus[nau - 1];
            a->data = realloc(a->data, a->size + 4 + nals[i].n);
            uint32_t be = CFSwapInt32HostToBig((uint32_t)nals[i].n);
            memcpy(a->data + a->size, &be, 4); memcpy(a->data + a->size + 4, nals[i].p, nals[i].n);
            a->size += 4 + nals[i].n;
        }
    }
    if (!sps || !pps || !nau || !aus[0].idr) { fprintf(stderr, "need SPS, PPS and a leading IDR\n"); return 1; }

    CMVideoFormatDescriptionRef fmt;
    const uint8_t *ps[2] = {sps, pps}; size_t psn[2] = {spsN, ppsN};
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, psn, 4, &fmt)) return 1;
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);

    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (hw) CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    else CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanFalse);
    int pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, pfn);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey,
                         CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    VTDecompressionOutputCallbackRecord cb = {callback, NULL};
    VTDecompressionSessionRef s;
    OSStatus st = VTDecompressionSessionCreate(NULL, fmt, spec, attrs, &cb, &s);
    if (st) { printf("{\"mode\":\"%s\",\"create_status\":%d}\n", argv[1] + 2, (int)st); return 1; }
    CFBooleanRef usingHw = NULL;
    VTSessionCopyProperty(s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &usingHw);

    // Warm-up: one GOP so session creation and first-IDR costs are excluded.
    double c0s = selfCpu(), c0v = serviceCpu(), t0 = now();
    long submitted = 0;
    for (int l = 0; l < loops; ++l) {
        for (size_t i = 0; i < nau; ++i) {
            CMBlockBufferRef bb; CMSampleBufferRef sb;
            CMBlockBufferCreateWithMemoryBlock(NULL, aus[i].data, aus[i].size, kCFAllocatorNull, NULL, 0, aus[i].size, 0, &bb);
            const size_t sz = aus[i].size;
            CMSampleTimingInfo ti = {CMTimeMake(1, 30), CMTimeMake((int64_t)submitted, 30), kCMTimeInvalid};
            CMSampleBufferCreateReady(NULL, bb, fmt, 1, 1, &ti, 1, &sz, &sb);
            double *stamp = malloc(2 * sizeof(double)); stamp[0] = now(); stamp[1] = (double)i;
            if (VTDecompressionSessionDecodeFrame(s, sb, 0, stamp, NULL)) atomic_fetch_add(&errors, 1);
            CFRelease(sb); CFRelease(bb);
            ++submitted;
            if (pace) { double next = t0 + submitted * pace; while (now() < next) usleep(500); }
        }
    }
    VTDecompressionSessionWaitForAsynchronousFrames(s);
    double el = now() - t0, cs = selfCpu() - c0s, cv = serviceCpu() - c0v;
    int fr = atomic_load(&frames);
    printf("{\"mode\":\"%s\",\"file\":\"%s\",\"size\":\"%dx%d\",\"using_hardware\":%s,\"frames\":%d,\"errors\":%d,"
           "\"elapsed_s\":%.3f,\"fps\":%.1f,\"cpu_self_s\":%.3f,\"cpu_vtservice_s\":%.3f,\"cpu_ms_per_frame\":%.2f,"
           "\"cpu_pct_of_one_core\":%.1f,\"latency_avg_ms\":%.2f,\"latency_max_ms\":%.2f%s}\n",
           argv[1] + 2, argv[2], dim.width, dim.height, usingHw == kCFBooleanTrue ? "true" : "false", fr,
           atomic_load(&errors), el, fr / el, cs, cv, fr ? (cs + cv) * 1000 / fr : 0, (cs + cv) / el * 100,
           fr ? sumLatency / 1000.0 / fr : 0, maxLatency / 1000.0, pace ? ",\"realtime\":true" : "");
    fprintf(stderr, "IDR latency ms:");
    for (int i = 0; i < idrN; ++i) fprintf(stderr, " %.1f", idrLat[i]);
    fprintf(stderr, "\n");
    VTDecompressionSessionInvalidate(s);
    return fr == submitted ? 0 : 1;
}
