#include "avd_accel.h"
#include "avc_parameters.h"
#include "avd_decode.h"
#include "avd_encode.h"
#include "avd_postprocess.h"
#include "avd_surface.h"
#include "phoenix_encode.h"
#include "PhxVideoDecode.h"
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// AMDRadeonVADriver2 8D3BA61E-7BC7-3BDF-ADB3-A7866360B6DE, x86_64h.
// The +0x09e0 factory returns a private pointer and 16 C callbacks.
// AppleGVA +0x241ab supplies 0x128 bytes for the renderer description.
enum { NotSupported = 1, BadParameter = 5, NoMemory = 7, Encode = 32, AVC = 0x800,
       VcnEngine = 4, CpuPostprocessEngine = 0x100, MaxContexts = 2,
       MaxPoolBuffers = 32, MaxCompletions = 1024,
       MaxBytesPerPool = 32 * 1024 * 1024 };

typedef struct {
    uint32_t selected, reserved;
    const io_service_t *services;
    uint32_t mask, padding;
    void *callbacks[2];
} DeviceSettings;

typedef struct {
    uint32_t type;
    char name[128];
    uint64_t bandwidth;
    uint32_t flags;
} Engine;

typedef struct { uint32_t format, count; } Format;

typedef struct {
    uint32_t operation;
    uint64_t function;
    uint32_t engine;
    uint64_t bandwidth, available;
    uint32_t inputCount;
    Format inputs[64];
    uint32_t outputCount;
    Format outputs[64];
    void *related;
} Function;

typedef struct {
    uint32_t engineCount;
    Engine **engines;
    uint32_t functionCount;
    Function **functions;
} Capabilities;

typedef struct {
    uint32_t renderer, rendererIDFromRegistry;
    Capabilities *capabilities;
    uint32_t version[3];
    char name[256];
} Renderer;

typedef struct Session Session;
typedef struct Accelerator Accelerator;
typedef struct { uint32_t kind; Accelerator *accelerator; } SessionSettings;
typedef struct { uint32_t count; void **items; } Array;
typedef struct { uint32_t commandID; const void *context; } Completion;
typedef struct {
    Completion *completion;
    uint32_t commandID, blocking, status, error;
} WaitRecord;
typedef struct { uint32_t count; WaitRecord *items; } WaitRequest;
typedef struct { uint32_t count; void **inputs; void **outputs; } ContextArray;
typedef struct { ContextArray *contexts; Capabilities *capabilities; } ContextRequest;
typedef struct ContextSettings {
    uint32_t operation;
    uint64_t function;
    uint32_t engine, reserved[3], width, height;
    uint32_t reservedAfterGeometry[4];
    struct ContextSettings *related;
} ContextSettings;
typedef struct {
    uint32_t id, flags, bytes, reserved[2], count;
    uint8_t **mappings;
} PoolRequest;
typedef struct { uint32_t id; void *value; } Property;
typedef struct { uint32_t count; Property *items; } Properties;

typedef int (*Operation)(Session *, void *);
typedef int (*ReleasePool)(Session *, uint32_t);
typedef struct {
    void *private;
    int (*create)(Session **, const SessionSettings *);
    int (*destroy)(Session *);
    Operation createContexts, destroyContexts, queryBuffer, allocatePool;
    ReleasePool releasePool;
    Operation querySurface, allocateSurfaces;
    ReleasePool releaseSurfaces;
    int (*submit)(Session *, const Array *, Array *);
    Operation wait, getStatus, getCapabilities, setProperties, getProperties;
} Interface;

struct Accelerator {
    Interface interface;
    io_service_t selectedService;
    atomic_uint references;
    Engine engine, cpuPostprocess;
    Function encode, decode, output, dither;
    Engine *engines[2];
    Function *functions[4];
    Capabilities capabilities;
};

struct Session {
    Accelerator *accelerator;
    pthread_mutex_t mutex;
    PhoenixVAConnection *connection;
    PhoenixVADecoder *decoder;
    uint32_t reportedRejections;
    CMVideoDimensions decodeSize, postprocessSize;
    PhoenixAVDMappedPool pools[3];
    PhoenixAVDSurfacePool pixelPool;
    Completion completed[MaxCompletions];
    unsigned nextCompletion;
};

_Static_assert(sizeof(WaitRecord) == 0x18 && offsetof(WaitRecord, status) == 0x10, "wait record layout");
_Static_assert(sizeof(Interface) == 0x88, "accelerator callback table");
_Static_assert(sizeof(DeviceSettings) == 0x28, "device settings");
_Static_assert(sizeof(Renderer) <= 0x128 && offsetof(Renderer, name) == 0x1c, "renderer output");
_Static_assert(sizeof(Engine) == 0x98, "engine descriptor");
_Static_assert(sizeof(Function) == 0x438 && offsetof(Function, outputCount) == 0x22c &&
               offsetof(Function, related) == 0x430, "function descriptor");
_Static_assert(offsetof(PoolRequest, mappings) == 0x18, "bitstream pool mapping");
_Static_assert(offsetof(ContextSettings, width) == 0x20 && offsetof(ContextSettings, related) == 0x38,
               "context geometry and output settings");

static os_log_t avdLog;
static pthread_once_t logOnce = PTHREAD_ONCE_INIT;
static void createLog(void) { avdLog = os_log_create("org.nullarc.phoenix.va-load-probe", "avd"); }

#if PHX_REFERENCE_DECODE_PROBE
#define referenceLog(...) os_log(avdLog, __VA_ARGS__)
#else
#define referenceLog(...) ((void)0)
#endif

static int createSession(Session **out, const SessionSettings *settings) {
    if (!out) return BadParameter;
    *out = NULL;
    if (!settings || !settings->accelerator) return BadParameter;
    Session *session = calloc(1, sizeof(*session));
    if (!session) return NoMemory;
    if (pthread_mutex_init(&session->mutex, NULL)) {
        free(session);
        return NoMemory;
    }
    session->accelerator = settings->accelerator;
    atomic_fetch_add_explicit(&session->accelerator->references, 1, memory_order_relaxed);
    *out = session;
    os_log(avdLog, "AVD create session kind=%{public}u", settings->kind);
    return 0;
}

static void freePool(PhoenixAVDMappedPool *pool) {
    for (uint32_t i = 0; i < pool->count; ++i) free(pool->buffers[i]);
    free(pool->buffers);
    *pool = (PhoenixAVDMappedPool){0};
}

static int destroySession(Session *session) {
    if (!session) return BadParameter;
    for (unsigned i = 0; i < 3; ++i) freePool(&session->pools[i]);
    PhoenixAVDReleaseSurfaces(&session->pixelPool, 14);
    PhoenixVAClose(session->connection);
    PhoenixVADecodeClose(session->decoder);
    PhoenixAVDDestroy(session->accelerator);
    pthread_mutex_destroy(&session->mutex);
    free(session);
    return 0;
}

static uint32_t contextOperation(const Accelerator *accelerator, const ContextSettings *context) {
    if (!context) return 0;
    if (context->engine == CpuPostprocessEngine && accelerator->cpuPostprocess.type &&
        context->operation == PhoenixAVDPostprocess && context->function == PhoenixAVDDither &&
        !context->related && PhoenixAVDCanCropNV12(context->width, context->height, context->width, context->height))
        return PhoenixAVDPostprocess;
    if (context->engine != VcnEngine || context->width > INT32_MAX || context->height > INT32_MAX)
        return 0;
    if (context->operation == Encode && context->function == AVC) return Encode;
    const ContextSettings *related = context->related;
    if (context->operation == PhoenixAVDDecode && context->function == PhoenixAVDDecodeAVC &&
        PhoenixAVDCanCropNV12(context->width, context->height, context->width, context->height) &&
        accelerator->decode.related && related && related->operation == PhoenixAVDOutput &&
        related->function == PhoenixAVDOutputNV12 && related->engine == VcnEngine && !related->related)
        return PhoenixAVDDecode;
    return 0;
}

static int createContexts(Session *session, void *arg) {
    ContextRequest *request = arg;
    if (!session || !request || !request->contexts) return BadParameter;
    ContextArray *list = request->contexts;
    os_log(avdLog, "AVD create contexts count=%{public}u", list->count);
    if (!list->count || list->count > MaxContexts || !list->inputs || !list->outputs)
        return NotSupported;
    const ContextSettings *inputs[MaxContexts];
    for (uint32_t i = 0; i < list->count; ++i) inputs[i] = list->inputs[i];
    memset(list->outputs, 0, list->count * sizeof(*list->outputs));
    request->capabilities = NULL;

    const ContextSettings *hardware = NULL;
    const ContextSettings *postprocess = NULL;
    for (uint32_t i = 0; i < list->count; ++i) {
        const ContextSettings *context = inputs[i];
        if (!context) return BadParameter;
        os_log(avdLog, "AVD create context op=%{public}u function=%{public}llu engine=%{public}u dimensions=%{public}ux%{public}u",
               context->operation, context->function, context->engine, context->width, context->height);
        uint32_t operation = contextOperation(session->accelerator, context);
        if (!operation) {
            referenceLog("AVD reference phase=context-admission op=%{public}u status=%{public}d",
                         context->operation, NotSupported);
            return NotSupported;
        }
        if (operation == PhoenixAVDPostprocess) {
            if (postprocess) return NotSupported;
            postprocess = context;
        } else {
            if (hardware) return NotSupported;
            hardware = context;
        }
    }

    pthread_mutex_lock(&session->mutex);
    int status = 0;
    CMVideoDimensions decodeSize = session->decodeSize, postSize = session->postprocessSize;
    if (hardware && hardware->operation == PhoenixAVDDecode)
        decodeSize = (CMVideoDimensions){(int32_t)hardware->width, (int32_t)hardware->height};
    if (postprocess)
        postSize = (CMVideoDimensions){(int32_t)postprocess->width, (int32_t)postprocess->height};
    if ((hardware && (session->connection || session->decoder)) || (postprocess && session->postprocessSize.width))
        status = NotSupported;
    if (decodeSize.width && postSize.width &&
        !PhoenixAVDCanCropNV12(decodeSize.width, decodeSize.height, postSize.width, postSize.height))
        status = NotSupported;
    if (!status && hardware) {
        CMVideoDimensions dims = {(int32_t)hardware->width, (int32_t)hardware->height};
        io_service_t service = session->accelerator->selectedService;
        OSStatus openStatus = hardware->operation == Encode ? PhoenixVAOpen(service, dims, &session->connection) :
            PhoenixVADecodeOpen(service, dims, &session->decoder);
        os_log(avdLog, "AVD hardware open result=%{public}d", (int)openStatus);
        if (openStatus) status = NotSupported;
    }
    if (!status) {
        session->decodeSize = decodeSize;
        session->postprocessSize = postSize;
        for (uint32_t i = 0; i < list->count; ++i)
            list->outputs[i] = inputs[i] == hardware ? (void *)session : &session->postprocessSize;
        request->capabilities = &session->accelerator->capabilities;
    }
    pthread_mutex_unlock(&session->mutex);
    return status;
}

static int destroyContexts(Session *session, void *arg) {
    Array *contexts = arg;
    if (!session || !contexts || !contexts->count || contexts->count > MaxContexts || !contexts->items)
        return BadParameter;
    pthread_mutex_lock(&session->mutex);
    bool hardware = false, postprocess = false;
    int status = 0;
    for (uint32_t i = 0; i < contexts->count; ++i) {
        const void *context = contexts->items[i];
        if (context == session && !hardware && (session->connection || session->decoder)) hardware = true;
        else if (context == &session->postprocessSize && !postprocess && session->postprocessSize.width)
            postprocess = true;
        else {
            status = BadParameter;
            break;
        }
    }
    if (!status) {
        if (hardware) {
            PhoenixVAClose(session->connection);
            session->connection = NULL;
            PhoenixVADecodeClose(session->decoder);
            session->decoder = NULL;
            session->reportedRejections = 0;
            session->decodeSize = (CMVideoDimensions){0};
        }
        if (postprocess) session->postprocessSize = (CMVideoDimensions){0};
        for (unsigned i = 0; i < MaxCompletions; ++i) {
            Completion *completion = &session->completed[i];
            if ((hardware && completion->context == session) ||
                (postprocess && completion->context == &session->postprocessSize)) completion->context = NULL;
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return status;
}

static int poolIndex(uint32_t id) {
    switch (id) {
        case 0: return 0;
        case 22: return 1;
        case 5: return 2;
        default: return -1;
    }
}

static int queryBuffer(Session *session, void *arg) {
    if (!session || !arg) return BadParameter;
    uint32_t operation;
    memcpy(&operation, arg, sizeof(operation));
    // AMDRadeonVADriver2 +0x5ce22 only writes these six output words.
    static const uint32_t encode[6] = {8, 8, 0, 0, 0, 0};
    static const uint32_t decode[6] = {64, 128, 0, 0, 0, 128};
    if (operation == PhoenixAVDDecode) {
        uint32_t kind;
        memcpy(&kind, (uint8_t *)arg + 12, sizeof(kind));
        referenceLog("AVD reference phase=buffer-query kind=%{public}u status=%{public}d",
                     kind, kind == 1 ? NotSupported : 0);
        if (kind == 1) return NotSupported;
        memcpy((uint8_t *)arg + 28, decode, sizeof(decode));
    } else if (operation == Encode) {
        memcpy((uint8_t *)arg + 28, encode, sizeof(encode));
    } else return NotSupported;
    return 0;
}

static int allocatePool(Session *session, void *arg) {
    PoolRequest *request = arg;
    if (!session || !request || !request->mappings || !request->bytes ||
        !request->count || request->count > MaxPoolBuffers) return BadParameter;
    int index = poolIndex(request->id);
    if (index < 0) return NotSupported;
    if (request->bytes > MaxBytesPerPool / request->count) return NoMemory;
    pthread_mutex_lock(&session->mutex);
    PhoenixAVDMappedPool *pool = &session->pools[index];
    int status = 0;
    if (pool->buffers) {
        if (pool->poolID != request->id || pool->count != request->count ||
            pool->bytesPerBuffer != request->bytes) status = BadParameter;
    } else {
        pool->poolID = request->id;
        pool->bytesPerBuffer = request->bytes;
        pool->buffers = calloc(request->count, sizeof(*pool->buffers));
        if (!pool->buffers) status = NoMemory;
        while (!status && pool->count < request->count) {
            void *mapping = NULL;
            if (posix_memalign(&mapping, 128, pool->bytesPerBuffer)) status = NoMemory;
            else {
                memset(mapping, 0, pool->bytesPerBuffer);
                pool->buffers[pool->count++] = mapping;
            }
        }
        if (status) freePool(pool);
    }
    if (!status) memcpy(request->mappings, pool->buffers, pool->count * sizeof(*pool->buffers));
    pthread_mutex_unlock(&session->mutex);
    return status;
}

static int releasePool(Session *session, uint32_t id) {
    int index = poolIndex(id);
    if (!session || index < 0) return BadParameter;
    pthread_mutex_lock(&session->mutex);
    PhoenixAVDMappedPool *pool = &session->pools[index];
    int status = 0;
    if (pool->buffers && pool->poolID != id) status = BadParameter;
    else freePool(pool);
    pthread_mutex_unlock(&session->mutex);
    return status;
}

static const Function *findFunction(const Capabilities *capabilities, const uint32_t *v) {
    for (unsigned i = 0; i < capabilities->functionCount; ++i) {
        const Function *f = capabilities->functions[i];
        if (v[0] == f->operation && v[1] == f->function && v[2] == f->engine) return f;
    }
    return NULL;
}

static int getProperties(Session *session, void *arg) {
    Properties *properties = arg;
    if (!session || !properties || (properties->count && !properties->items)) return BadParameter;
    Capabilities *caps = &session->accelerator->capabilities;
    for (uint32_t i = 0; i < properties->count; ++i) {
        Property *p = &properties->items[i];
        if (p->id == 9 && p->value) {
            const uint32_t *v = p->value;
            if (!findFunction(caps, v)) {
                referenceLog("AVD reference phase=bandwidth-query op=%{public}u status=%{public}d",
                             v[0], NotSupported);
                return NotSupported;
            }
        } else if (p->id != 14) {
            os_log(avdLog, "AVD unsupported get property %{public}u", p->id);
            return NotSupported;
        }
    }
    for (uint32_t i = 0; i < properties->count; ++i) {
        Property *p = &properties->items[i];
        if (p->id == 14) {
            // AMD2 +0x53e4 replaces the entry's pointer. AppleGVA +0x3c21d
            // reads it back and scans the retained accelerator's function list.
            p->value = caps;
            referenceLog("AVD reference phase=capability-query engines=%{public}u functions=%{public}u",
                         caps->engineCount, caps->functionCount);
        } else {
            // AMD2 +0x5d2ce writes bandwidth to the fourth word for property 9.
            uint32_t *v = p->value;
            v[3] = (uint32_t)findFunction(caps, v)->bandwidth;
        }
    }
    return 0;
}

static int unsupported(Session *session, void *arg) {
    (void)session;
    (void)arg;
    return NotSupported;
}

static int allocateSurfaces(Session *session, void *arg) {
    if (!session) return BadParameter;
    pthread_mutex_lock(&session->mutex);
    int result = PhoenixAVDAllocateSurfaces(&session->pixelPool, arg);
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static int releaseSurfaces(Session *session, uint32_t id) {
    if (!session) return BadParameter;
    pthread_mutex_lock(&session->mutex);
    int result = PhoenixAVDReleaseSurfaces(&session->pixelPool, id);
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static int submit(Session *session, const Array *commands, Array *results) {
    if (!session || !commands || !results || commands->count != results->count)
        return BadParameter;
    if (!commands->count) return 0;
    if (!commands->items || !results->items || commands->count > MaxCompletions)
        return BadParameter;
    memset(results->items, 0, results->count * sizeof(*results->items));
    pthread_mutex_lock(&session->mutex);
    int status = (session->connection || session->decoder || session->postprocessSize.width) ? 0 : NotSupported;
    if (session->decoder || session->postprocessSize.width)
        referenceLog("AVD reference phase=submit count=%{public}u", commands->count);
    for (uint32_t i = 0; !status && i < commands->count; ++i) {
        const PhoenixAVDCommand *cmd = commands->items[i];
        if (!cmd || !cmd->picture) status = BadParameter;
        else if (cmd->context == session && session->decoder) {
            if (cmd->operation != PhoenixAVDDecode || cmd->function != PhoenixAVDDecodeAVC) status = NotSupported;
        } else if (cmd->context == session && session->connection) {
            const PhoenixAVDAVCPicture *picture = cmd->picture;
            if (cmd->operation != Encode || cmd->function != AVC) status = NotSupported;
            else if (poolIndex(picture->ring.poolID) < 0) status = BadParameter;
        } else if (cmd->context == &session->postprocessSize && session->postprocessSize.width) {
            if (cmd->operation != PhoenixAVDPostprocess || cmd->function != PhoenixAVDDither) status = NotSupported;
        } else status = BadParameter;
        if (status && (session->decoder || session->postprocessSize.width))
            referenceLog("AVD reference phase=command-admission index=%{public}u status=%{public}d", i, status);
    }
    for (uint32_t i = 0; !status && i < commands->count; ++i) {
        const PhoenixAVDCommand *cmd = commands->items[i];
        if (cmd->context == &session->postprocessSize) {
            status = PhoenixAVDDitherNV12(cmd);
        } else if (session->decoder) {
            status = PhoenixAVDDecodeAVCFrame(session->decoder, &session->reportedRejections, cmd, session->pools, 2);
        } else {
            const PhoenixAVDAVCPicture *picture = cmd->picture;
            int index = poolIndex(picture->ring.poolID);
            status = PhoenixAVDEncodeAVC(session->connection, cmd, &session->pools[index]);
        }
        if (status) {
            os_log(avdLog, "AVD submit failed op=%{public}u function=%{public}llu command=%{public}u status=%{public}d",
                   cmd->operation, cmd->function, cmd->commandID, status);
            break;
        }
        // AMD +0x74a8 recycles 1024 session-owned handles. AppleGVA +0xad4f7
        // pairs a returned handle with the submitted command ID in its wait.
        Completion *completion = &session->completed[session->nextCompletion];
        *completion = (Completion){.commandID = cmd->commandID, .context = cmd->context};
        results->items[i] = completion;
        session->nextCompletion = (session->nextCompletion + 1) % MaxCompletions;
        if (session->decoder || session->postprocessSize.width)
            referenceLog("AVD reference phase=completion op=%{public}u command=%{public}u", cmd->operation, cmd->commandID);
    }
    pthread_mutex_unlock(&session->mutex);
    return status;
}

static int waitForCommands(Session *session, void *arg) {
    WaitRequest *request = arg;
    if (!session || !request || !request->items) return BadParameter;
    pthread_mutex_lock(&session->mutex);
    int status = 0;
    for (uint32_t i = 0; i < request->count; ++i) {
        WaitRecord *item = &request->items[i];
        uintptr_t offset = (uintptr_t)item->completion - (uintptr_t)session->completed;
        if (offset >= sizeof(session->completed) || offset % sizeof(Completion) ||
            !item->completion->context || item->completion->commandID != item->commandID) {
            item->status = 3;
            item->error = BadParameter;
            status = BadParameter;
            break;
        }
        // Submissions finish before returning a completion to the caller.
        item->status = 1;
        item->error = 0;
    }
    if (session->decoder || session->postprocessSize.width)
        referenceLog("AVD reference phase=wait count=%{public}u status=%{public}d", request->count, status);
    pthread_mutex_unlock(&session->mutex);
    return status;
}

int PhoenixAVDCreate(void **out, const void *settings, void *capabilities) {
    if (!out) return BadParameter;
    *out = NULL;
    const DeviceSettings *device = settings;
    if (!device || !capabilities || device->selected >= 32 || !device->services ||
        !(device->mask & (1u << device->selected)) || !device->services[device->selected]) return NotSupported;
    pthread_once(&logOnce, createLog);
    Accelerator *a = calloc(1, sizeof(*a));
    if (!a) return NoMemory;
    a->selectedService = device->services[device->selected];
    if (IOObjectRetain(a->selectedService) != KERN_SUCCESS) {
        free(a);
        return NotSupported;
    }
    atomic_init(&a->references, 1);
    a->interface = (Interface){
        .private = a, .create = createSession, .destroy = destroySession,
        .createContexts = createContexts, .destroyContexts = destroyContexts,
        .queryBuffer = queryBuffer, .allocatePool = allocatePool, .releasePool = releasePool,
        .querySurface = PhoenixAVDQuerySurface, .allocateSurfaces = allocateSurfaces, .releaseSurfaces = releaseSurfaces,
        .submit = submit, .wait = waitForCommands, .getStatus = unsupported,
        .getCapabilities = unsupported, .setProperties = unsupported, .getProperties = getProperties,
    };
    // The renderer offered by NMXAccel uses one VCN engine. Function 0x800
    // selects the AMD2 NV12 -> H.264 raw-bitstream branch.
    const uint64_t bandwidth = (uint64_t)PhxAVCWidth * PhxAVCHeight * 25;
    a->engine = (Engine){.type = VcnEngine, .name = "Phoenix VCN", .bandwidth = bandwidth};
    a->encode = (Function){.operation = Encode, .function = AVC, .engine = VcnEngine,
        .bandwidth = bandwidth, .available = bandwidth,
        .inputCount = 1, .inputs = {{0x4e563132, 1}}, .outputCount = 1, .outputs = {{0x72617762, 1}}};
    a->engines[0] = &a->engine;
    a->functions[0] = &a->encode;
    a->capabilities = (Capabilities){1, a->engines, 1, a->functions};
    // A fixed-vector backend alone can be advertised only by the reference probe.
    bool stream = PhoenixVADecodeStreamAvailable(a->selectedService) && PhoenixAVDCompatibleAppleGVA();
    if (stream || (PHX_REFERENCE_DECODE_PROBE && PhoenixVADecodeAvailable(a->selectedService))) {
        // AMD2 entries 0 and 20 form the rawb -> NV12 -> display surface path.
        // This value is the nominal admission bandwidth for the reference size.
        const uint64_t pixels = stream ? (uint64_t)PHX_VIDEO_DECODE_STREAM_MAX_WIDTH * PHX_VIDEO_DECODE_STREAM_MAX_HEIGHT * 30 :
                                         (uint64_t)PHX_VIDEO_DECODE_WIDTH * PHX_VIDEO_DECODE_HEIGHT * 25;
        a->output = (Function){.operation = PhoenixAVDOutput, .function = PhoenixAVDOutputNV12,
            .engine = VcnEngine, .bandwidth = pixels, .available = pixels,
            .inputCount = 1, .inputs = {{0x4e563132, 1}}, .outputCount = 1, .outputs = {{0x4e563132, 1}}};
        a->decode = (Function){.operation = PhoenixAVDDecode, .function = PhoenixAVDDecodeAVC,
            .engine = VcnEngine, .bandwidth = pixels, .available = pixels,
            .inputCount = 1, .inputs = {{0x72617762, 1}}, .outputCount = 1, .outputs = {{0x4e563132, 1}},
            .related = &a->output};
        // AppleGVA +0x3bcf4 resolves the dither function's engine name by ID.
        // Use a driver-local ID and name for the synchronous CPU passthrough.
        a->cpuPostprocess = (Engine){.type = CpuPostprocessEngine, .name = "Phoenix CPU NV12 Passthrough",
            .bandwidth = pixels};
        a->dither = (Function){.operation = PhoenixAVDPostprocess, .function = PhoenixAVDDither,
            .engine = CpuPostprocessEngine, .bandwidth = pixels, .available = pixels,
            .inputCount = 1, .inputs = {{0x4e563132, 1}}, .outputCount = 1, .outputs = {{0x4e563132, 1}}};
        a->engines[1] = &a->cpuPostprocess;
        a->functions[1] = &a->decode;
        a->functions[2] = &a->output;
        a->functions[3] = &a->dither;
        a->capabilities.engineCount = 2;
        a->capabilities.functionCount = 4;
        a->engine.bandwidth = pixels;
    }
    referenceLog("AVD reference phase=publish decode_available=%{public}d", a->decode.related != NULL);
    Renderer *renderer = capabilities;
    *renderer = (Renderer){.renderer = 0x01020000, .capabilities = &a->capabilities,
                           .version = {1, 0, 0}, .name = "Phoenix VCN"};
    CFTypeRef id = IORegistryEntryCreateCFProperty(a->selectedService, CFSTR("IOVARendererID"), NULL, 0);
    int64_t publishedID;
    if (id) {
        // AMD2 sets the second renderer word when it obtains the ID from the selected service.
        if (CFGetTypeID(id) == CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)id, kCFNumberSInt64Type, &publishedID) &&
            (publishedID == 0x01020000 || publishedID == 0x01080001)) {
            renderer->renderer = (uint32_t)publishedID;
            renderer->rendererIDFromRegistry = 1;
        }
        CFRelease(id);
    }
    *out = a;
    os_log(avdLog, "AVD created accelerator renderer=0x%{public}08x", renderer->renderer);
    return 0;
}

int PhoenixAVDDestroy(void *accelerator) {
    Accelerator *a = accelerator;
    if (a && atomic_fetch_sub_explicit(&a->references, 1, memory_order_acq_rel) == 1) {
        IOObjectRelease(a->selectedService);
        free(a);
    }
    return 0;
}
