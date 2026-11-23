#ifndef PHX_AVC_IO_H
#define PHX_AVC_IO_H
#include <CommonCrypto/CommonDigest.h>
#include "phx_avc.h"
#include "PhxVcnUserABI.h"

// Build 64 clients use the fixed selector and its original bitstream capacity.
static inline size_t phxAVCOutputCapacity(uint32_t width, uint32_t height) {
    return width == PHXN_VCN_WIDTH && height == PHXN_VCN_HEIGHT ? PHXN_VCN_H264_CAPACITY
                                                               : phxnVcnBitstreamCapacity(width, height);
}

OSStatus PhxAVCSubmit(io_connect_t connection, uint32_t width, uint32_t height,
                      const uint8_t *nv12, uint8_t *annexB, size_t *bytes);
OSStatus PhxAVCPackNV12(CVPixelBufferRef image, size_t width, size_t height,
                        uint8_t *packed, size_t capacity);
OSStatus PhxAVCCopyAnnexBWithDigest(io_connect_t connection, CVPixelBufferRef image,
                                   CFDataRef *accessUnit, uint8_t digest[CC_SHA256_DIGEST_LENGTH]);
#endif
