#ifndef PHOENIX_AVD_SURFACE_H
#define PHOENIX_AVD_SURFACE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *mapping;
    size_t length;
} PhoenixAVDSurfacePool;

struct Session;
int PhoenixAVDQuerySurface(struct Session *session, void *request);
// The caller serializes allocation, release and teardown of this session-owned pool.
int PhoenixAVDAllocateSurfaces(PhoenixAVDSurfacePool *pool, const void *request);
int PhoenixAVDReleaseSurfaces(PhoenixAVDSurfacePool *pool, uint32_t id);

#endif
