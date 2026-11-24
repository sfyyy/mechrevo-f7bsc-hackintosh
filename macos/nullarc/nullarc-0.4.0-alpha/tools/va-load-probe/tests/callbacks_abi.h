#ifndef PHOENIX_TEST_CALLBACKS_ABI_H
#define PHOENIX_TEST_CALLBACKS_ABI_H

#include <IOKit/IOKitLib.h>

typedef struct { uint32_t selected, reserved; io_service_t *services; uint32_t mask, pad; void *callbacks[2]; } Device;
typedef struct { uint32_t kind; void *accelerator; } SessionSettings;
typedef struct { uint32_t count; void **items; } Array;
typedef struct { uint32_t count; void **inputs, **outputs; } ContextArray;
typedef struct { ContextArray *contexts; void *capabilities; } ContextRequest;
typedef struct Context {
    uint32_t operation;
    uint64_t function;
    uint32_t engine, reserved[3], w, h, reservedAfterGeometry[4];
    struct Context *related;
} Context;
typedef struct { uint32_t id, flags, bytes, reserved[2], count; uint8_t **mappings; } Pool;
typedef struct { void *completion; uint32_t commandID, blocking, status, error; } WaitRecord;
typedef struct { uint32_t count; WaitRecord *items; } Wait;
typedef struct { uint32_t renderer, rendererIDFromRegistry; void *capabilities; uint32_t version[3]; char name[256]; } Renderer;
typedef int (*Operation)(void *, void *);
typedef int (*ReleasePool)(void *, uint32_t);
typedef struct {
    void *privateData;
    int (*create)(void **, const SessionSettings *);
    int (*destroy)(void *);
    Operation createContexts, destroyContexts, queryBuffer, allocatePool;
    ReleasePool releasePool;
    Operation querySurface, allocateSurfaces;
    ReleasePool releaseSurfaces;
    int (*submit)(void *, const Array *, Array *);
    Operation wait, getStatus, getCapabilities, setProperties, getProperties;
} Interface;

#endif
