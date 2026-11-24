/* Phoenix video-context ABI v1. One synchronous 160x128 NV12 -> H.264 IDR. */
#ifndef PHX_VIDEO_AVC_H
#define PHX_VIDEO_AVC_H

#include "../phxbringup/PhxVcnUserABI.h"

#define PHX_VIDEO_AVC_PROPERTY "PhxVideoAVCABI"
#define PHX_VIDEO_AVC_VERSION 1u
/* IOConnectCallStructMethod on this accelerator's video context only. No scalars.
 * Input: PHXN_VCN_NV12_BYTES. Output capacity: PHXN_VCN_H264_CAPACITY.
 * The returned byte count is the length of one completed Annex B access unit. */
#define PHX_VIDEO_ENCODE_AVC 0x50484131u

#ifdef KERNEL
#define PHX_VIDEO_AVC_FUNCTION "PhxEncodeAVCFrameV1"
#endif
#endif
