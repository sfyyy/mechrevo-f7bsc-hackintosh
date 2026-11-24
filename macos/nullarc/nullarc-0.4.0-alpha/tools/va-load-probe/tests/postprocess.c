#include "../avd_postprocess.h"
#include "PhxVideoDecode.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static CVPixelBufferRef makePixel(size_t width, size_t height, OSType format, size_t alignment) {
    CFNumberRef rowAlignment = CFNumberCreate(NULL, kCFNumberCFIndexType, &alignment);
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferBytesPerRowAlignmentKey};
    const void *values[] = {empty, rowAlignment};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pixel = NULL;
    assert(!CVPixelBufferCreate(NULL, width, height, format, attrs, &pixel));
    CFRelease(attrs);
    CFRelease(empty);
    CFRelease(rowAlignment);
    return pixel;
}

static uint8_t pattern(size_t plane, size_t row, size_t col) {
    return (uint8_t)(31 * plane + 73 * row + 17 * col);
}

static void fill(CVPixelBufferRef pixel, size_t width, bool withPattern, uint8_t padding) {
    assert(!CVPixelBufferLockBaseAddress(pixel, 0));
    for (size_t plane = 0; plane < 2; ++plane) {
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        uint8_t *row = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pixel, plane); ++y, row += pitch) {
            memset(row, padding, pitch);
            if (withPattern)
                for (size_t x = 0; x < width; ++x) row[x] = pattern(plane, y, x);
        }
    }
    assert(!CVPixelBufferUnlockBaseAddress(pixel, 0));
}

static void check(CVPixelBufferRef pixel, size_t width, bool withPattern, uint8_t padding) {
    assert(!CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly));
    for (size_t plane = 0; plane < 2; ++plane) {
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        const uint8_t *row = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pixel, plane); ++y, row += pitch) {
            for (size_t x = 0; x < pitch; ++x)
                assert(row[x] == (withPattern && x < width ? pattern(plane, y, x) : padding));
        }
    }
    assert(!CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly));
}

static size_t testCopy(size_t width, size_t sourceHeight, size_t destinationHeight) {
    CVPixelBufferRef source = makePixel(width, sourceHeight,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 64);
    CVPixelBufferRef destination = makePixel(width, destinationHeight,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 512);
    CVPixelBufferRef fullRange = makePixel(width, destinationHeight,
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 64);
    assert(CVPixelBufferGetBytesPerRowOfPlane(source, 0) != CVPixelBufferGetBytesPerRowOfPlane(destination, 0));
    fill(source, width, true, 0xa3);
    fill(destination, width, false, 0xd4);

    PhoenixAVDDitherPicture picture = {
        .source = CVPixelBufferGetIOSurface(source),
        .destination = CVPixelBufferGetIOSurface(destination)
    };
    PhoenixAVDCommand command = {
        .operation = PhoenixAVDPostprocess,
        .function = PhoenixAVDDither,
        .picture = &picture
    };
    for (size_t i = 0; i < 3; ++i) {
        picture.enabled[i] = 1;
        assert(PhoenixAVDDitherNV12(&command) == 1);
        picture.enabled[i] = 0;
    }
    command.next = &command;
    assert(PhoenixAVDDitherNV12(&command) == 1);
    command.next = NULL;

    picture.source = CVPixelBufferGetIOSurface(fullRange);
    assert(PhoenixAVDDitherNV12(&command) == 3);
    check(destination, width, false, 0xd4);
    picture.source = CVPixelBufferGetIOSurface(source);
    picture.destination = NULL;
    assert(PhoenixAVDDitherNV12(&command) == 0);
    picture.destination = picture.source;
    assert(PhoenixAVDDitherNV12(&command) == 0);
    picture.destination = CVPixelBufferGetIOSurface(destination);
    assert(PhoenixAVDDitherNV12(&command) == 0);
    check(destination, width, true, 0xd4);
    check(source, width, true, 0xa3);

    size_t visibleBytes = width * destinationHeight * 3 / 2;
    CFRelease(source);
    CFRelease(destination);
    CFRelease(fullRange);
    return visibleBytes;
}

static void testRejectGeometry(size_t sourceWidth, size_t sourceHeight, size_t width, size_t height) {
    CVPixelBufferRef source = makePixel(sourceWidth, sourceHeight, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 64);
    CVPixelBufferRef destination = makePixel(width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 512);
    fill(source, sourceWidth, true, 0xa3);
    fill(destination, width, false, 0xd4);
    PhoenixAVDDitherPicture picture = {
        .source = CVPixelBufferGetIOSurface(source),
        .destination = CVPixelBufferGetIOSurface(destination)
    };
    PhoenixAVDCommand command = {
        .operation = PhoenixAVDPostprocess,
        .function = PhoenixAVDDither,
        .picture = &picture
    };
    assert(PhoenixAVDDitherNV12(&command) == 3);
    check(destination, width, false, 0xd4);
    check(source, sourceWidth, true, 0xa3);
    CFRelease(source);
    CFRelease(destination);
}

int main(void) {
    assert(testCopy(PHX_VIDEO_DECODE_WIDTH, PHX_VIDEO_DECODE_HEIGHT, PHX_VIDEO_DECODE_HEIGHT) == 622080);
    assert(testCopy(1920, 1088, 1080) == 3110400);
    assert(testCopy(16, 2, 2) == 48);
    testRejectGeometry(1920, 1088, 1920, 1072);
    testRejectGeometry(1920, 1080, 1920, 1088);
    testRejectGeometry(1920, 1088, 1918, 1080);
    puts("NV12 identity and 8-row bottom crop pass for 622080 and 3110400 visible bytes");
    return 0;
}
