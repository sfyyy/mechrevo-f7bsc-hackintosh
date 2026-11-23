#include "phx_avc_io.h"
#include <VideoToolbox/VTErrors.h>

// PhxBringup's native client handshake is shared with the graphics driver.
enum { nativeClient = 0x4e34384e, hello = 0, abiVersion = 1 };
_Static_assert(PhxAVCWidth == PHXN_VCN_WIDTH && PhxAVCHeight == PHXN_VCN_HEIGHT &&
               PhxAVCInputBytes == PHXN_VCN_NV12_BYTES && PhxAVCMaxBytes == PHXN_VCN_H264_CAPACITY,
               "encoder format must match the kext");

static OSStatus ioStatus(kern_return_t kr) {
    switch (kr) {
    case KERN_SUCCESS: return noErr;
    case kIOReturnBadArgument: return kVTParameterErr;
    case kIOReturnUnsupported: return kVTVideoEncoderNotAvailableNowErr;
    case kIOReturnNotPrivileged:
    case kIOReturnNotPermitted: return kVTVideoEncoderAuthorizationErr;
    case kIOReturnNoMemory: return kVTAllocationFailedErr;
    case kIOReturnBusy:
    case kIOReturnNotReady:
    case kIOReturnNoDevice:
    case kIOReturnOffline: return kVTVideoEncoderNotAvailableNowErr;
    default: return kVTVideoEncoderMalfunctionErr;
    }
}

OSStatus PhxAVCOpenForDimensions(CMVideoDimensions dimensions, io_connect_t *connection) {
    if (!connection) return kVTParameterErr;
    *connection = IO_OBJECT_NULL;
    if (!phxnVcnSizeSupported(dimensions.width, dimensions.height)) return kVTParameterErr;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!service) return kVTCouldNotFindVideoEncoderErr;
    io_connect_t client = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), nativeClient, &client);
    IOObjectRelease(service);
    if (kr) return ioStatus(kr);
    const uint64_t request[] = {abiVersion, 0};
    uint64_t reply[4] = {0};
    uint32_t count = 4;
    kr = IOConnectCallScalarMethod(client, hello, request, 2, reply, &count);
    OSStatus status = ioStatus(kr);
    if (!status && (count != 4 || reply[0] != abiVersion || reply[1] < PHXN_VCN_MIN_BUILD))
        status = kVTCouldNotFindVideoEncoderErr;
    if (!status && reply[1] < PHXN_VCN_SIZED_MIN_BUILD &&
        (dimensions.width != PhxAVCWidth || dimensions.height != PhxAVCHeight))
        status = kVTParameterErr;
    if (!status) {
        uint64_t ready = 0;
        count = 1;
        kr = IOConnectCallScalarMethod(client, PHXN_SEL_VCN_ENCODE_READY, NULL, 0, &ready, &count);
        status = ioStatus(kr);
        if (!status && (count != 1 || ready != 1)) status = kVTVideoEncoderNotAvailableNowErr;
    }
    if (status) IOServiceClose(client);
    else *connection = client;
    return status;
}

OSStatus PhxAVCOpen(io_connect_t *connection) {
    return PhxAVCOpenForDimensions((CMVideoDimensions){PhxAVCWidth, PhxAVCHeight}, connection);
}

OSStatus PhxAVCSubmit(io_connect_t connection, uint32_t width, uint32_t height,
                      const uint8_t *nv12, uint8_t *annexB, size_t *bytes) {
    if (width == PHXN_VCN_WIDTH && height == PHXN_VCN_HEIGHT)
        return ioStatus(IOConnectCallStructMethod(connection, PHXN_SEL_VCN_ENCODE_H264,
                                                  nv12, PHXN_VCN_NV12_BYTES, annexB, bytes));
    const uint64_t dimensions[] = {width, height};
    return ioStatus(IOConnectCallMethod(connection, PHXN_SEL_VCN_ENCODE_H264_SIZED, dimensions, 2,
                                        nv12, phxnVcnPackedBytes(width, height), NULL, NULL, annexB, bytes));
}
