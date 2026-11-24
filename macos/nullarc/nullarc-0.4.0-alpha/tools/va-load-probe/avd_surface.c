#include "avd_surface.h"
#include "avd_decode.h"

#include <sys/mman.h>
#include <unistd.h>

enum { NotSupported = 1, InvalidFormat = 3, BadParameter = 5, NoMemory = 7,
       AVC = 0x800, Encode = 32, NV12 = 0x4e563132, AVCPixelPool = 14, MaxBufferCount = 32 };

// AMD2 GetBufferRequirement writes through the four output arrays supplied by the caller.
typedef struct {
    uint32_t operation, function, reserved, kind, width, height, format, reserved2, planeCount;
    uint32_t *widths, *heights, *pitches, *alignments;
} PixelRequirements;

_Static_assert(offsetof(PixelRequirements, widths) == 40 && offsetof(PixelRequirements, alignments) == 64,
               "AMD2 pixel requirement arrays");

int PhoenixAVDQuerySurface(struct Session *session, void *arg) {
    PixelRequirements *r = arg;
    if (!session || !r) return BadParameter;
    bool encode = r->operation == Encode && r->function == AVC;
    bool decode = r->operation == PhoenixAVDDecode && r->function == PhoenixAVDDecodeAVC;
    bool output = r->operation == PhoenixAVDOutput && r->function == PhoenixAVDOutputNV12;
    if (!encode && !decode && !output) return NotSupported;
    if ((encode && r->kind == 1) || (decode && r->kind == 0)) return BadParameter;
    if (r->format != NV12 || r->planeCount != 2) return InvalidFormat;
    if (!r->width || !r->height || r->width > UINT32_MAX - 255 || r->height > UINT32_MAX - 15 ||
        !r->widths || !r->heights || !r->pitches || !r->alignments) return BadParameter;
    const uint32_t pitch = (r->width + 255) & ~255u;
    const uint32_t height = (r->height + 15) & ~15u;
    // AMD2 +0x5e66d reports UV in sample pairs for decode/output. Pitch is bytes.
    r->widths[0] = r->width;
    r->widths[1] = encode ? r->width : r->width / 2;
    r->heights[0] = height;
    r->heights[1] = height / 2;
    r->pitches[0] = r->pitches[1] = pitch;
    r->alignments[0] = r->alignments[1] = 256;
    return 0;
}

typedef struct {
    uint8_t reserved[48];
    void **outAddress;
} PixelEntry;

typedef struct {
    uint32_t id, flags, width, height, count;
    PixelEntry *entries;
} PixelPoolRequest;

_Static_assert(sizeof(PixelEntry) == 56 && offsetof(PixelPoolRequest, entries) == 24,
               "AMD2 AllocatePixelBufferPool layout");

int PhoenixAVDReleaseSurfaces(PhoenixAVDSurfacePool *pool, uint32_t id) {
    if (!pool || id != AVCPixelPool) return BadParameter;
    if (pool->mapping) munmap(pool->mapping, pool->length);
    *pool = (PhoenixAVDSurfacePool){0};
    return 0;
}

int PhoenixAVDAllocateSurfaces(PhoenixAVDSurfacePool *pool, const void *arg) {
    const PixelPoolRequest *r = arg;
    if (!pool || !r || r->id != AVCPixelPool || !r->entries || !r->count ||
        r->count > MaxBufferCount || !r->width || !r->height) return BadParameter;
    if (r->flags) return NotSupported;
    for (uint32_t i = 0; i < r->count; ++i)
        if (!r->entries[i].outAddress) return BadParameter;

    // AppleGVA copies out per-entry CPU addresses and discards its descriptors.
    // Phoenix's AVC path uses these as host-side auxiliary buffers. Reserve
    // whole pages so entries remain distinct and individually page-aligned.
    const uint64_t bytes = (uint64_t)r->width * r->height;
    const size_t maxBytes = 32u * 1024u * 1024u;
    if (bytes > maxBytes) return NoMemory;
    const size_t page = (size_t)getpagesize();
    const size_t stride = ((size_t)bytes + page - 1) / page * page;
    if (stride > maxBytes / r->count) return NoMemory;
    const size_t length = stride * r->count;
    void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return NoMemory;

    PhoenixAVDReleaseSurfaces(pool, r->id);
    *pool = (PhoenixAVDSurfacePool){mapping, length};
    for (uint32_t i = 0; i < r->count; ++i)
        *r->entries[i].outAddress = (uint8_t *)mapping + stride*i;
    return 0;
}
