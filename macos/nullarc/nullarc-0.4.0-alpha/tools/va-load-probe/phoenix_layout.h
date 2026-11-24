#ifndef PHOENIX_LAYOUT_H
#define PHOENIX_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Where the kernel should write the decoded picture, and whether it can write
// straight into the destination surface or needs a packed bounce buffer.
typedef struct PhoenixSurfaceLayout {
    bool direct;
    size_t chromaOffset;
} PhoenixSurfaceLayout;

// Both planes of an IOSurface share one allocation and one stride, but the
// chroma plane starts after the padded luma plane, not after the visible rows:
// a 1080-line surface places it at row 1088, because video decoders ask for
// eight rows of extended pixels bottom. Deriving the offset from the visible
// height alone therefore rejects every padded surface and costs a full bounce
// copy per frame. Read the offset off the planes instead.
// anyOffset == false keeps the historical packed-offset rule for A/B.
static inline PhoenixSurfaceLayout phoenixSurfaceLayout(
    bool stream, size_t surfaceWidth, size_t surfaceHeight,
    const uint8_t *plane0, size_t stride0, const uint8_t *plane1, size_t stride1,
    const uint8_t *allocationBase, size_t allocationBytes, bool anyOffset) {
    const size_t packed = surfaceHeight * stride0;
    PhoenixSurfaceLayout layout = {false, packed};
    if (!stream || !plane0 || !plane1 || plane1 <= plane0) return layout;
    if (stride0 != stride1 || (stride0 & 1u) != 0 || stride0 < surfaceWidth || (surfaceHeight & 1u) != 0)
        return layout;
    const size_t delta = (size_t)(plane1 - plane0);
    const size_t head = allocationBase && plane0 >= allocationBase ? (size_t)(plane0 - allocationBase) : 0;
    const size_t need = delta + stride0 * (surfaceHeight >> 1);
    if ((delta & 1u) != 0 || (anyOffset ? delta < packed : delta != packed)) return layout;
    if (allocationBytes && head + need > allocationBytes) return layout;
    layout.direct = true;
    layout.chromaOffset = delta;
    return layout;
}

#endif
