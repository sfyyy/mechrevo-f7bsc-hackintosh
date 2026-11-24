#include "avd_postprocess.h"
#include <string.h>

enum { NotSupported = 1, InvalidFormat = 3, BadParameter = 5 };

_Static_assert(offsetof(PhoenixAVDDitherPicture, enabled) == 0x1c, "AppleGVA dither enable fields");

bool PhoenixAVDCanCropNV12(size_t sourceWidth, size_t sourceHeight, size_t width, size_t height) {
    if (!width || !height || width > INT32_MAX || sourceHeight > INT32_MAX ||
        (width & 1) || (height & 1) || sourceWidth != width || sourceHeight < height) return false;
    // The decode surface can include the bottom of the final H.264 macroblock row.
    return sourceHeight == height || (sourceHeight % 16 == 0 && sourceHeight - height < 16);
}

static bool validNV12(IOSurfaceRef surface) {
    size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    if (!width || !height || (width & 1) || (height & 1) ||
        IOSurfaceGetPixelFormat(surface) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        IOSurfaceGetPlaneCount(surface) != 2)
        return false;
    for (size_t plane = 0; plane < 2; ++plane) {
        if (IOSurfaceGetWidthOfPlane(surface, plane) != (width >> plane) ||
            IOSurfaceGetHeightOfPlane(surface, plane) != (height >> plane) ||
            IOSurfaceGetBytesPerElementOfPlane(surface, plane) != (1u << plane) ||
            IOSurfaceGetElementWidthOfPlane(surface, plane) != 1 ||
            IOSurfaceGetElementHeightOfPlane(surface, plane) != 1 ||
            IOSurfaceGetBytesPerRowOfPlane(surface, plane) < width)
            return false;
    }
    return true;
}

static uint8_t *visiblePlane(IOSurfaceRef surface, size_t plane, size_t rowBytes, size_t rows) {
    uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
    uintptr_t addr = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, plane);
    size_t size = IOSurfaceGetAllocSize(surface);
    size_t pitch = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
    if (!base || !rows || rowBytes > pitch || addr < base || addr - base > size) return NULL;
    size_t remaining = size - (addr - base);
    if (remaining < rowBytes || rows - 1 > (remaining - rowBytes) / pitch)
        return NULL;
    return (uint8_t *)addr;
}

int PhoenixAVDDitherNV12(const PhoenixAVDCommand *command) {
    if (!command || !command->picture) return BadParameter;
    if (command->operation != PhoenixAVDPostprocess || command->function != PhoenixAVDDither || command->next)
        return NotSupported;
    const PhoenixAVDDitherPicture *p = command->picture;
    if (p->enabled[0] || p->enabled[1] || p->enabled[2]) return NotSupported;
    if (!p->source) return BadParameter;
    if (!validNV12(p->source)) return InvalidFormat;
    // pushPicturePrepareCmd can omit the output surface for an identity transform.
    if (!p->destination || IOSurfaceGetID(p->source) == IOSurfaceGetID(p->destination)) return 0;

    if (!validNV12(p->destination)) return InvalidFormat;
    size_t width = IOSurfaceGetWidth(p->destination), height = IOSurfaceGetHeight(p->destination);
    if (!PhoenixAVDCanCropNV12(IOSurfaceGetWidth(p->source), IOSurfaceGetHeight(p->source), width, height))
        return InvalidFormat;

    // Always acquire a pair in surface-ID order across simultaneous sessions.
    bool sourceFirst = IOSurfaceGetID(p->source) < IOSurfaceGetID(p->destination);
    IOSurfaceRef first = sourceFirst ? p->source : p->destination;
    IOSurfaceRef second = sourceFirst ? p->destination : p->source;
    IOSurfaceLockOptions firstOptions = sourceFirst ? kIOSurfaceLockReadOnly : 0;
    IOSurfaceLockOptions secondOptions = sourceFirst ? 0 : kIOSurfaceLockReadOnly;
    if (IOSurfaceLock(first, firstOptions, NULL)) return BadParameter;
    if (IOSurfaceLock(second, secondOptions, NULL)) {
        IOSurfaceUnlock(first, firstOptions, NULL);
        return BadParameter;
    }
    uint8_t *src[2], *dst[2];
    int status = 0;
    for (size_t i = 0; i < 2; ++i) {
        size_t rows = height >> i;
        src[i] = visiblePlane(p->source, i, width, rows);
        dst[i] = visiblePlane(p->destination, i, width, rows);
        if (!src[i] || !dst[i]) status = BadParameter;
    }
    if (!status) {
        for (size_t i = 0; i < 2; ++i) {
            size_t sourcePitch = IOSurfaceGetBytesPerRowOfPlane(p->source, i);
            size_t destinationPitch = IOSurfaceGetBytesPerRowOfPlane(p->destination, i);
            for (size_t row = 0; row < (height >> i); ++row)
                memcpy(dst[i] + row * destinationPitch, src[i] + row * sourcePitch, width);
        }
    }
    if (IOSurfaceUnlock(second, secondOptions, NULL)) status = BadParameter;
    if (IOSurfaceUnlock(first, firstOptions, NULL)) status = BadParameter;
    return status;
}
