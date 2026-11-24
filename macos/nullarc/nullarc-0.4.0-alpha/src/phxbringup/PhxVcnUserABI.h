/* Synchronous Phoenix H.264 encoder methods on the N48N user client after HELLO. */
#ifndef PHX_VCN_USER_ABI_H
#define PHX_VCN_USER_ABI_H

#include <stdint.h>

#define PHXN_VCN_MIN_BUILD 64u
#define PHXN_VCN_WIDTH 160u
#define PHXN_VCN_HEIGHT 128u
#define PHXN_VCN_NV12_BYTES (PHXN_VCN_WIDTH * PHXN_VCN_HEIGHT * 3u / 2u)
#define PHXN_VCN_H264_CAPACITY 28672u

/* IOConnectCallStructMethod. Supply exactly PHXN_VCN_NV12_BYTES of packed video-range
   NV12 (Y followed by interleaved UV, no padding) and PHXN_VCN_H264_CAPACITY bytes
   of output storage. On success the returned length covers one Annex B SPS/PPS/IDR. */
#define PHXN_SEL_VCN_ENCODE_H264 31u
/* IOConnectCallScalarMethod. Zero inputs, one output. One when the initialized
   VCN can accept a frame. Readiness may change before the next submission. */
#define PHXN_SEL_VCN_ENCODE_READY 32u

/* Build 65. IOConnectCallMethod, scalars [width,height], no scalar outputs,
   packed video-range NV12 input and one Annex-B output. Selector 31 remains
   available for applications using the build 64 fixed-size ABI. */
#define PHXN_VCN_SIZED_MIN_BUILD 65u
#define PHXN_SEL_VCN_ENCODE_H264_SIZED 33u
#define PHXN_VCN_MIN_DIMENSION 128u
#define PHXN_VCN_MAX_LONG_EDGE 2560u
#define PHXN_VCN_MAX_SHORT_EDGE 1440u
static inline int phxnVcnSizeSupported(uint32_t w, uint32_t h) {
    return w >= PHXN_VCN_MIN_DIMENSION && h >= PHXN_VCN_MIN_DIMENSION &&
           w <= PHXN_VCN_MAX_LONG_EDGE && h <= PHXN_VCN_MAX_LONG_EDGE &&
           (w <= PHXN_VCN_MAX_SHORT_EDGE || h <= PHXN_VCN_MAX_SHORT_EDGE) && !((w | h) & 1u);
}
static inline uint32_t phxnVcnPackedBytes(uint32_t w, uint32_t h) {
    return phxnVcnSizeSupported(w, h) ? w * h * 3u / 2u : 0u;
}
static inline uint32_t phxnVcnBitstreamCapacity(uint32_t w, uint32_t h) {
    if (!phxnVcnSizeSupported(w, h)) return 0u;
    /* Two bytes per coded pixel plus header space, in whole pages. */
    uint32_t codedPixels = ((w + 15u) & ~15u) * ((h + 15u) & ~15u);
    return (2u * codedPixels + 4096u + 4095u) & ~4095u;
}

#endif
