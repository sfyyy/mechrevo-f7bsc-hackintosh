/* IOAccel video-context contract for the Phoenix VCN 4.0.2 reference IDR.
 * Input: the five libdrm VCL NAL units, each with a 3-byte Annex B prefix.
 * SPS/PPS are described by the fixed VCN parameter block, not sent here.
 * Output: 480 rows of Y followed by 240 rows of interleaved UV, pitch 1024.
 * This revision's exact slice payload is checked before any firmware submit. */
#ifndef PHX_VIDEO_DECODE_H
#define PHX_VIDEO_DECODE_H

#define PHX_VIDEO_DECODE_PROPERTY "PhxVideoDecodeReferenceABI"
#define PHX_VIDEO_DECODE_VERSION 1u
#define PHX_VIDEO_DECODE_AVC 0x50484431u
#define PHX_VIDEO_DECODE_INPUT_BYTES 1396u
#define PHX_VIDEO_DECODE_WIDTH 864u
#define PHX_VIDEO_DECODE_HEIGHT 480u
#define PHX_VIDEO_DECODE_PITCH 1024u
#define PHX_VIDEO_DECODE_OUTPUT_BYTES (PHX_VIDEO_DECODE_PITCH * PHX_VIDEO_DECODE_HEIGHT * 3u / 2u)

/* Video-context ABI for a validated stream backend. Input is one Annex B
 * access unit. The exact output capacity is the SPS-visible packed NV12 size.
 * The backend parses each AU and owns per-context reference state. */
#define PHX_VIDEO_DECODE_STREAM_PROPERTY "PhxVideoDecodeStreamABI"
#define PHX_VIDEO_DECODE_STREAM_VERSION 1u
#define PHX_VIDEO_DECODE_STREAM_AVC 0x50484432u
#define PHX_VIDEO_DECODE_STREAM_MAX_INPUT (1024u * 1024u)
#define PHX_VIDEO_DECODE_STREAM_MAX_WIDTH 1920u
#define PHX_VIDEO_DECODE_STREAM_MAX_HEIGHT 1088u
#define PHX_VIDEO_DECODE_STREAM_MAX_OUTPUT (1920u * 1088u * 3u / 2u)

#ifdef KERNEL
#define PHX_VIDEO_DECODE_FUNCTION "PhxDecodeReferenceIDRV1"
// p1: owning IOAccel video context, p2: PhxVideoDecodeStreamInput,
// p3: output IOMemoryDescriptor, p4: returned uint32_t packed byte count.
// All arguments are borrowed synchronously. Close takes p1 alone and drops
// references for this owner. A backend must bound output to the descriptor.
#define PHX_VIDEO_DECODE_STREAM_FUNCTION "PhxDecodeAVCStreamV1"
#define PHX_VIDEO_DECODE_STREAM_CLOSE "PhxCloseAVCStreamV1"
struct PhxVideoDecodeStreamInput {
    const void *bytes;
    unsigned int length;
    // Destination surface layout. pitch is the row stride in bytes and covers
    // both planes; chromaOffset is the byte offset of the UV plane from the
    // first byte handed to the backend. Zero means a packed buffer whose pitch
    // equals the visible width and whose UV plane follows the luma plane.
    unsigned int pitch;
    unsigned int chromaOffset;
};
#endif
#endif
