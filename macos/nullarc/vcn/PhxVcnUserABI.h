/* Synchronous Phoenix H.264 encoder methods on the N48N user client after HELLO. */
#ifndef PHX_VCN_USER_ABI_H
#define PHX_VCN_USER_ABI_H

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

#endif
