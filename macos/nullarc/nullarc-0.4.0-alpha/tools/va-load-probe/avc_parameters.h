#ifndef PHOENIX_AVC_PARAMETERS_H
#define PHOENIX_AVC_PARAMETERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PhoenixAVCParameterCapacity = 512 };
bool PhoenixAVDCompatibleAppleGVA(void);
// Input is Apple's x86_64 0xc18-byte SPS and 0x488-byte PPS. Both returned NALs
// include four-byte start codes. Failure leaves the buffer untouched and *written zero.
bool PhoenixAVDExportAVCParameters(const uint32_t *sps, const uint32_t *pps,
                                   uint8_t *annexB, size_t capacity, size_t *written);
#endif
