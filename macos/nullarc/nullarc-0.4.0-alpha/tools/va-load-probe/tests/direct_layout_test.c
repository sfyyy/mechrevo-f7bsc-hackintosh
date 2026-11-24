/* Checks the plugin's direct-write decision against real CoreVideo surfaces.
 *
 * The kernel writes a decoded picture straight into a destination surface when
 * both planes share one allocation and one stride. Video decoders ask for eight
 * rows of extended pixels bottom, so a 1080-line surface keeps its chroma plane
 * at row 1088 and the historical "chroma right after the visible rows" rule
 * rejected every such surface. This test pins the visible-height rule, the
 * offset taken from the planes themselves, the build 75 fallback and the
 * non-contiguous rejection.
 */
#include "../phoenix_layout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

/* 420v surface the way a video decoder asks for it: eight rows of extended
 * pixels bottom, 64-byte row alignment. */
static CVPixelBufferRef makeSurface(size_t width, size_t height, int extendedBottom) {
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, empty);
    if (extendedBottom > 0) {
        CFNumberRef bottom = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &extendedBottom);
        CFDictionarySetValue(attributes, kCVPixelBufferExtendedPixelsBottomKey, bottom);
        CFRelease(bottom);
    }
    int alignment = 64;
    CFNumberRef align = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &alignment);
    CFDictionarySetValue(attributes, kCVPixelBufferBytesPerRowAlignmentKey, align);
    CFRelease(align);
    CVPixelBufferRef surface = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &surface);
    CFRelease(attributes);
    CFRelease(empty);
    return surface;
}

static PhoenixSurfaceLayout layoutOf(CVPixelBufferRef surface, bool anyOffset) {
    CVPixelBufferLockBaseAddress(surface, 0);
    const uint8_t *plane0 = CVPixelBufferGetBaseAddressOfPlane(surface, 0);
    const uint8_t *plane1 = CVPixelBufferGetBaseAddressOfPlane(surface, 1);
    const PhoenixSurfaceLayout layout = phoenixSurfaceLayout(
        true, CVPixelBufferGetWidth(surface), CVPixelBufferGetHeight(surface),
        plane0, CVPixelBufferGetBytesPerRowOfPlane(surface, 0),
        plane1, CVPixelBufferGetBytesPerRowOfPlane(surface, 1),
        CVPixelBufferGetBaseAddress(surface), CVPixelBufferGetDataSize(surface), anyOffset);
    CVPixelBufferUnlockBaseAddress(surface, 0);
    return layout;
}

static size_t chromaDelta(CVPixelBufferRef surface) {
    CVPixelBufferLockBaseAddress(surface, 0);
    const ptrdiff_t delta = CVPixelBufferGetBaseAddressOfPlane(surface, 1) -
                            CVPixelBufferGetBaseAddressOfPlane(surface, 0);
    CVPixelBufferUnlockBaseAddress(surface, 0);
    return (size_t)delta;
}

static void checkPadded(size_t width, size_t height, size_t expectedDelta) {
    CVPixelBufferRef surface = makeSurface(width, height, 8);
    char label[96];
    if (!surface) {
        snprintf(label, sizeof label, "%zux%zu padded: creation failed", width, height);
        check(false, label);
        return;
    }
    const size_t delta = chromaDelta(surface);
    const size_t packed = height * CVPixelBufferGetBytesPerRowOfPlane(surface, 0);
    snprintf(label, sizeof label, "%zux%zu padded surface: chroma plane at %zu (packed would be %zu)",
             width, height, delta, packed);
    check(delta > packed && (expectedDelta == 0 || delta == expectedDelta), label);

    const PhoenixSurfaceLayout wide = layoutOf(surface, true);
    snprintf(label, sizeof label, "%zux%zu padded: direct write at the real offset %zu", width, height, delta);
    check(wide.direct && wide.chromaOffset == delta, label);

    const PhoenixSurfaceLayout strict = layoutOf(surface, false);
    snprintf(label, sizeof label, "%zux%zu padded: build 75 rule falls back to the bounce buffer", width, height);
    check(!strict.direct && strict.chromaOffset == packed, label);

    /* The kernel needs the mapped destination to cover the chroma plane. */
    CVPixelBufferLockBaseAddress(surface, 0);
    const size_t head = (size_t)((const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(surface, 0) -
                                 (const uint8_t *)CVPixelBufferGetBaseAddress(surface));
    const size_t need = wide.chromaOffset + CVPixelBufferGetBytesPerRowOfPlane(surface, 0) * (height >> 1);
    const size_t available = CVPixelBufferGetDataSize(surface);
    CVPixelBufferUnlockBaseAddress(surface, 0);
    snprintf(label, sizeof label, "%zux%zu padded: %zu + %zu bytes fit the %zu-byte mapping", width, height, head, need, available);
    check(head + need <= available, label);
    CVPixelBufferRelease(surface);
}

static void checkPacked(void) {
    CVPixelBufferRef surface = makeSurface(1920, 1080, 0);
    if (!surface) { check(false, "1920x1080 packed: creation failed"); return; }
    const size_t delta = chromaDelta(surface);
    const size_t packed = 1080 * (size_t)CVPixelBufferGetBytesPerRowOfPlane(surface, 0);
    check(delta == packed, "1920x1080 packed surface keeps the chroma plane right after the visible rows");
    const PhoenixSurfaceLayout strict = layoutOf(surface, false);
    check(strict.direct && strict.chromaOffset == packed, "1920x1080 packed: build 75 rule still writes directly");
    const PhoenixSurfaceLayout wide = layoutOf(surface, true);
    check(wide.direct && wide.chromaOffset == delta, "1920x1080 packed: new rule agrees with the planes");
    CVPixelBufferRelease(surface);
}

static void checkNonContiguous(void) {
    const size_t width = 1920, height = 1080, pitch = 1920;
    const size_t luma = pitch * height, chroma = pitch * (height / 2);
    uint8_t *memory = calloc(1, luma + chroma);
    if (!memory) { check(false, "non-contiguous: allocation failed"); return; }
    void *planes[2] = {memory + luma, memory}; /* chroma before luma */
    size_t widths[2] = {width, width / 2}, heights[2] = {height, height / 2};
    size_t strides[2] = {pitch, pitch};
    CVPixelBufferRef surface = NULL;
    CVReturn status = CVPixelBufferCreateWithPlanarBytes(kCFAllocatorDefault, width, height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, 0, 2, planes, widths, heights, strides,
        NULL, NULL, NULL, &surface);
    check(status == kCVReturnSuccess && surface != NULL, "non-contiguous planar buffer is created");
    if (surface) {
        const PhoenixSurfaceLayout layout = layoutOf(surface, true);
        check(!layout.direct, "non-contiguous planes are never written directly");
        CVPixelBufferRelease(surface);
    }
    free(memory);
}

int main(void) {
    printf("direct layout test\n");
    checkPadded(1920, 1080, 2088960); /* what the VT decoder pool reports, row 1088 */
    checkPadded(1280, 720, 0);
    checkPadded(960, 540, 0);
    checkPadded(864, 480, 0);
    checkPacked();
    checkNonContiguous();
    if (failures) {
        printf("FAIL %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
