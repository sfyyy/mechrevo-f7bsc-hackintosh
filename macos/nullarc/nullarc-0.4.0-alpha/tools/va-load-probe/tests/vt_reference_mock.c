// Offline VT plumbing test. Only --mock builds link these transport functions.
#include "phoenix_decode.h"
#include "PhxVideoDecode.h"
#include <CommonCrypto/CommonDigest.h>
#include <VideoToolbox/VTErrors.h>
#include <stdlib.h>
#include <string.h>
struct PhoenixVADecoder { bool open; };

OSStatus PhoenixVADecodeOpen(io_service_t service, CMVideoDimensions dims, PhoenixVADecoder **out) {
    (void)service;
    if (dims.width != PHX_VIDEO_DECODE_WIDTH || dims.height != PHX_VIDEO_DECODE_HEIGHT || !out) return kVTParameterErr;
    *out = calloc(1, sizeof(**out));
    if (!*out) return kVTAllocationFailedErr;
    (*out)->open = true;
    return noErr;
}

void PhoenixVADecodeClose(PhoenixVADecoder *decoder) { free(decoder); }

OSStatus PhoenixVADecodeFrame(PhoenixVADecoder *decoder, const uint8_t *slices, size_t bytes, CVPixelBufferRef pixel) {
    // Digest of the five caller-supplied libdrm VCL NALs after prefix conversion.
    static const uint8_t expected[CC_SHA256_DIGEST_LENGTH] = {
        0x31,0x48,0x39,0x9b,0x1d,0x76,0x75,0x95,0x5d,0xfc,0x5a,0xe8,0x0c,0x87,0x04,0xc8,
        0x2a,0x05,0xd0,0xad,0x1d,0xa8,0xb6,0x18,0x22,0xc6,0xad,0xf8,0x9e,0x1c,0x13,0xaf
    };
    if (!decoder || !decoder->open || !slices || bytes != PHX_VIDEO_DECODE_INPUT_BYTES) return kVTParameterErr;
    uint8_t digest[sizeof(expected)];
    CC_SHA256(slices, (CC_LONG)bytes, digest);
    if (memcmp(digest, expected, sizeof(expected))) return kVTVideoDecoderUnsupportedDataFormatErr;
    if (!pixel || CVPixelBufferGetPlaneCount(pixel) != 2 ||
        CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetWidth(pixel) != PHX_VIDEO_DECODE_WIDTH || CVPixelBufferGetHeight(pixel) != PHX_VIDEO_DECODE_HEIGHT ||
        CVPixelBufferLockBaseAddress(pixel, 0)) return kVTParameterErr;
    // Distinct synthetic marker planes deliberately cannot match decoded reference pixels.
    for (size_t i = 0; i < 2; ++i) {
        uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixel, i);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, i);
        if (!base || stride < PHX_VIDEO_DECODE_WIDTH || CVPixelBufferGetHeightOfPlane(pixel,i) != (PHX_VIDEO_DECODE_HEIGHT>>i)) {
            CVPixelBufferUnlockBaseAddress(pixel, 0);
            return kVTParameterErr;
        }
        for (size_t row = 0; row < (PHX_VIDEO_DECODE_HEIGHT>>i); ++row)
            memset(base + row*stride, i == 0 ? 0x53 : 0x71, PHX_VIDEO_DECODE_WIDTH);
    }
    return CVPixelBufferUnlockBaseAddress(pixel, 0);
}
