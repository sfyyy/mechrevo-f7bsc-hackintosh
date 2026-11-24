#ifndef PHOENIX_AVD_ENCODE_H
#define PHOENIX_AVD_ENCODE_H

#include <IOSurface/IOSurface.h>
#include <IOKit/IOKitLib.h>
#include <stddef.h>
#include <stdint.h>
#include "phoenix_encode.h"

typedef struct {
    uint32_t poolID;
    uint32_t bufferIndex;
    uint32_t capacity;
    uint32_t readOffset;
    uint32_t writeOffset;
} PhoenixAVDRing;

typedef struct {
    uint8_t reserved[108];
    uint32_t encodedBits;
} PhoenixAVDAVCStatistics;

// AMD2 AVC picture parameters for function 0x800 on x86_64 macOS 26.
typedef struct {
    PhoenixAVDRing ring;
    uint8_t reservedBeforeSurface[12];
    IOSurfaceRef surface;
    uint8_t reservedBeforeStats[776];
    PhoenixAVDAVCStatistics *statistics;
} PhoenixAVDAVCPicture;

typedef struct PhoenixAVDCommand {
    uint32_t operation;
    uint32_t reserved;
    uint64_t function;
    uint32_t commandID;
    uint32_t reservedAfterID;
    void *context;
    void *picture;
    void *dependency;
    struct PhoenixAVDCommand *next;
} PhoenixAVDCommand;

// CPU mappings allocated by the driver and returned to AppleGVA through
// AllocateBitstreamBufferPool. The caller serializes access to the pool.
typedef struct {
    uint32_t poolID;
    uint32_t bytesPerBuffer;
    uint32_t count;
    uint8_t **buffers;
} PhoenixAVDMappedPool;

#ifdef __cplusplus
extern "C" {
#endif
int PhoenixAVDEncodeAVC(PhoenixVAConnection *connection, const PhoenixAVDCommand *command,
                        const PhoenixAVDMappedPool *pool);
#ifdef __cplusplus
}
#endif
#endif
