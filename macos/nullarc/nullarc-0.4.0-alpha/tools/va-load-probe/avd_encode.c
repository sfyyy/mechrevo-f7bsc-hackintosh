#include "avd_encode.h"
#include "phoenix_encode.h"

#include <string.h>

// AMDRadeonVADriver2 +0x47e1c and +0x5185e agree with the AppleGVA
// AMD2 outputFrame accessors at +0xabcca and +0xabcde.
_Static_assert(offsetof(PhoenixAVDCommand, picture) == 32, "command picture offset");
_Static_assert(offsetof(PhoenixAVDAVCPicture, surface) == 32, "input IOSurface offset");
_Static_assert(offsetof(PhoenixAVDAVCPicture, statistics) == 816, "picture statistics offset");
_Static_assert(offsetof(PhoenixAVDAVCStatistics, encodedBits) == 108, "encoded bit count offset");

int PhoenixAVDEncodeAVC(PhoenixVAConnection *connection, const PhoenixAVDCommand *command,
                        const PhoenixAVDMappedPool *pool) {
    if (!connection || !command || command->operation != 32 || command->function != 0x800 ||
        !command->picture || !pool || !pool->buffers)
        return 5;

    PhoenixAVDAVCPicture *picture = command->picture;
    PhoenixAVDRing *ring = &picture->ring;
    if (!picture->surface || !picture->statistics || ring->poolID != pool->poolID ||
        ring->bufferIndex >= pool->count || !pool->buffers[ring->bufferIndex] ||
        !ring->capacity || ring->capacity > pool->bytesPerBuffer ||
        ring->readOffset >= ring->capacity || ring->writeOffset != ring->readOffset)
        return 5;

    CVPixelBufferRef image = NULL;
    if (CVPixelBufferCreateWithIOSurface(NULL, picture->surface, NULL, &image) != kCVReturnSuccess)
        return 1;
    CFDataRef accessUnit = NULL;
    OSStatus status = PhoenixVAEncodeFrame(connection, image, &accessUnit);
    CVPixelBufferRelease(image);
    if (status != noErr || !accessUnit) {
        if (accessUnit) CFRelease(accessUnit);
        return 1;
    }

    CFIndex size = CFDataGetLength(accessUnit);
    if (size <= 0 || size >= ring->capacity || size > UINT32_MAX / 8) {
        CFRelease(accessUnit);
        return 1;
    }

    uint8_t *base = pool->buffers[ring->bufferIndex];
    const UInt8 *bytes = CFDataGetBytePtr(accessUnit);
    uint32_t tail = ring->capacity - ring->readOffset;
    uint32_t length = (uint32_t)size;
    uint32_t first = length < tail ? length : tail;
    memcpy(base + ring->readOffset, bytes, first);
    if (first < length) memcpy(base, bytes + first, length - first);
    ring->writeOffset = (uint32_t)(((uint64_t)ring->readOffset + length) % ring->capacity);
    picture->statistics->encodedBits = length * 8;
    CFRelease(accessUnit);
    return 0;
}
