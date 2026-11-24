#ifndef PHOENIX_VA_ENCODE_H
#define PHOENIX_VA_ENCODE_H

#include "../vcn/phx_avc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PhoenixVAConnection PhoenixVAConnection;
OSStatus PhoenixVAOpen(io_service_t accelerator, CMVideoDimensions dimensions, PhoenixVAConnection **out);
void PhoenixVAClose(PhoenixVAConnection *connection);
// A successful frame returns one owned Annex B access unit.
OSStatus PhoenixVAEncodeFrame(PhoenixVAConnection *connection, CVPixelBufferRef image, CFDataRef *accessUnit);

#ifdef __cplusplus
}
#endif
#endif
