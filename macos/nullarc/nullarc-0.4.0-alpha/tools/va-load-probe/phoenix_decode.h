#ifndef PHOENIX_VA_DECODE_H
#define PHOENIX_VA_DECODE_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PhoenixVADecoder PhoenixVADecoder;
bool PhoenixVADecodeAvailable(io_service_t accelerator);
bool PhoenixVADecodeStreamAvailable(io_service_t accelerator);
bool PhoenixVADecodeHasStream(const PhoenixVADecoder *decoder);
bool PhoenixVADecodeHasReference(const PhoenixVADecoder *decoder);
OSStatus PhoenixVADecodeOpen(io_service_t accelerator, CMVideoDimensions dimensions,
                             PhoenixVADecoder **out);
void PhoenixVADecodeClose(PhoenixVADecoder *decoder);
// The caller serializes use of the context. Input contains just the IDR slices
// in Annex B order; the caller validates the out-of-band picture parameters.
OSStatus PhoenixVADecodeFrame(PhoenixVADecoder *decoder, const uint8_t *annexB, size_t length,
                              CVPixelBufferRef destination);
// Submit on the same context throughout a reference chain. The first IDR
// contains SPS/PPS. Visible dimensions come from the parsed SPS. Destination
// may have the visible or the macroblock-aligned dimensions for this session.
OSStatus PhoenixVADecodeAccessUnit(PhoenixVADecoder *decoder, const uint8_t *annexB, size_t length,
                                   CMVideoDimensions visible, CVPixelBufferRef destination);

#endif
