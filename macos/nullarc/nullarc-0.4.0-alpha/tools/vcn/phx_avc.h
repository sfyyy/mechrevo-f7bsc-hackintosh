// Synchronous Phoenix AVC encoder for video-range and full-range NV12 pixel buffers.
#ifndef PHX_AVC_H
#define PHX_AVC_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build 64 frame geometry, retained for existing callers.
enum { PhxAVCWidth = 160, PhxAVCHeight = 128,
       PhxAVCInputBytes = PhxAVCWidth * PhxAVCHeight * 3 / 2, PhxAVCMaxBytes = 0x7000 };

// The caller releases a successful connection with IOServiceClose.
OSStatus PhxAVCOpen(io_connect_t *connection);

// Also checks that the loaded driver supports the requested session dimensions.
OSStatus PhxAVCOpenForDimensions(CMVideoDimensions dimensions, io_connect_t *connection);

// Synchronously encodes one NV12 image to an Annex B access unit (SPS/PPS/IDR).
// On success the caller owns the CFData. The frame's timing is tracked by the caller.
OSStatus PhxAVCCopyAnnexB(io_connect_t connection, CVPixelBufferRef image, CFDataRef *accessUnit);

// The caller owns the returned, independently decodable AVC sample.
OSStatus PhxAVCEncode(io_connect_t connection, CVPixelBufferRef image, CMTime pts,
                      CMTime duration, CMSampleBufferRef *sample);

#ifdef __cplusplus
}
#endif
#endif
