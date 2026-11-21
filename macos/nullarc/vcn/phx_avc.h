// Synchronous Phoenix AVC encoder for 160x128 video-range NV12 pixel buffers.
#ifndef PHX_AVC_H
#define PHX_AVC_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PhxAVCWidth = 160, PhxAVCHeight = 128,
       PhxAVCInputBytes = PhxAVCWidth * PhxAVCHeight * 3 / 2, PhxAVCMaxBytes = 0x7000 };

// The caller releases a successful connection with IOServiceClose.
OSStatus PhxAVCOpen(io_connect_t *connection);

// The caller owns the returned, independently decodable AVC sample.
OSStatus PhxAVCEncode(io_connect_t connection, CVPixelBufferRef image, CMTime pts,
                      CMTime duration, CMSampleBufferRef *sample);

#ifdef __cplusplus
}
#endif
#endif
