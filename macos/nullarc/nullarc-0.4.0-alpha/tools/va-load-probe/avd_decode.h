#ifndef PHOENIX_AVD_DECODE_H
#define PHOENIX_AVD_DECODE_H

#include "avd_encode.h"
#include "phoenix_decode.h"

enum { PhoenixAVDDecode = 1, PhoenixAVDDecodeAVC = 8, PhoenixAVDOutput = 16,
       PhoenixAVDOutputNV12 = 1 };

typedef struct { uint32_t length, offset, headerBits; } PhoenixAVDSlice;
typedef struct {
    uint8_t reserved[0x260];
    const uint32_t *sps, *pps;
    uint32_t poolID, bufferIndex, sliceCount;
    void *pictureHeader, *sliceHeaders;
    const PhoenixAVDSlice *const *slices;
} PhoenixAVDDecodePicture;

typedef struct {
    uint32_t sourcePool, sourceIndex, targetPool, targetIndex;
    IOSurfaceRef surface;
} PhoenixAVDDecodeTarget;

int PhoenixAVDDecodeAVCFrame(PhoenixVADecoder *decoder, uint32_t *reportedRejections, const PhoenixAVDCommand *command,
                             const PhoenixAVDMappedPool *pools, size_t poolCount);
#endif
