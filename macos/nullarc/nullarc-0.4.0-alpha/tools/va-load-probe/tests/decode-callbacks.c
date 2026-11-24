// Exercise the VA callback table with either the mock transport or the local bundle.
#include "callbacks_abi.h"
#include "avd_accel.h"
#include "avd_decode.h"
#include "avd_postprocess.h"
#include "PhxVideoDecode.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static bool openDecoder, failFrame;
static unsigned calls;
static uint8_t fixture[PHX_VIDEO_DECODE_INPUT_BYTES];
static IOSurfaceRef surface;
#if !PHX_TEST_BUNDLE
static bool available, allowHD;
static unsigned openAttempts;
static CFTypeRef mockRendererID;
static io_service_t mockService;
struct PhoenixVADecoder { int unused; };
static PhoenixVADecoder decoder;

CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t service, CFStringRef key,
                                          CFAllocatorRef allocator, IOOptionBits options) {
    (void)allocator;
    (void)options;
    assert(service == mockService);
    return mockRendererID && CFEqual(key, CFSTR("IOVARendererID")) ? CFRetain(mockRendererID) : NULL;
}

static void checkRenderer(CFTypeRef property, uint32_t expectedID, bool fromRegistry) {
    io_service_t services[] = {IO_OBJECT_NULL, mockService};
    Device device = {.selected = 1, .mask = 2, .services = services};
    union { Renderer renderer; uint8_t storage[0x128]; } description = {0};
    void *accelerator = NULL;
    mockRendererID = property;
    assert(!PhoenixAVDCreate(&accelerator, &device, &description));
    assert(description.renderer.renderer == expectedID);
    assert(description.renderer.rendererIDFromRegistry == fromRegistry);
    assert(!PhoenixAVDDestroy(accelerator));
    mockRendererID = NULL;
}

bool PhoenixVADecodeAvailable(io_service_t service) { return service && available; }
bool PhoenixVADecodeStreamAvailable(io_service_t service) { (void)service; return false; }
bool PhoenixVADecodeHasStream(const PhoenixVADecoder *d) { (void)d; return false; }
bool PhoenixVADecodeHasReference(const PhoenixVADecoder *d) { return d != NULL; }
OSStatus PhoenixVADecodeAccessUnit(PhoenixVADecoder *d, const uint8_t *bytes, size_t n,
                                   CMVideoDimensions visible, CVPixelBufferRef pixel) {
    (void)d; (void)bytes; (void)n; (void)visible; (void)pixel;
    assert(0 && "fixed reference decoder received a stream");
    return -1;
}
OSStatus PhoenixVADecodeOpen(io_service_t service, CMVideoDimensions dims, PhoenixVADecoder **out) {
    assert(service && available && !openDecoder);
    ++openAttempts;
    *out = NULL;
    bool reference = dims.width == PHX_VIDEO_DECODE_WIDTH && dims.height == PHX_VIDEO_DECODE_HEIGHT;
    if (!reference && !(allowHD && dims.width == 1920 && dims.height == 1088)) return -1;
    openDecoder = true;
    *out = &decoder;
    return noErr;
}
void PhoenixVADecodeClose(PhoenixVADecoder *d) {
    if (d) { assert(d == &decoder && openDecoder); openDecoder = false; }
}
OSStatus PhoenixVADecodeFrame(PhoenixVADecoder *d, const uint8_t *bytes, size_t n, CVPixelBufferRef pixel) {
    assert(openDecoder && d == &decoder && n == sizeof(fixture) && !memcmp(bytes, fixture, n));
    assert(CVPixelBufferGetIOSurface(pixel) == surface);
    ++calls;
    return failFrame ? -1 : noErr;
}
#endif

typedef struct { uint32_t id; void *value; } Property;
typedef struct { uint32_t count; Property *items; } Properties;
typedef struct { uint32_t op; uint64_t fn; uint32_t engine; uint8_t other[0x41c]; void *related; } Function;
typedef struct { uint32_t type; char name[128]; uint64_t bandwidth; uint32_t flags; } Engine;
typedef struct { uint32_t engines; Engine **engineList; uint32_t count; Function **functions; } Caps;
_Static_assert(offsetof(Function, related) == 0x430, "linked descriptor");

static Function *findFunction(const Caps *caps, uint32_t op, uint64_t fn) {
    for (uint32_t i = 0; i < caps->count; ++i) {
        Function *f = caps->functions[i];
        if (f->op == op && f->fn == fn) return f;
    }
    return NULL;
}

static void checkEngine(const Caps *caps, uint32_t type) {
    for (uint32_t i = 0; i < caps->engines; ++i) {
        const Engine *engine = caps->engineList[i];
        if (engine->type == type) {
            assert(engine->name[0] && memchr(engine->name, 0, sizeof(engine->name)));
            return;
        }
    }
    assert(0 && "function's engine is missing");
}

static void checkSurface(Interface *ops, void *session) {
    uint32_t widths[2], heights[2], pitches[2], alignments[2];
    struct {
        uint32_t op, fn, reserved, kind, w, h, format, reserved2, planes;
        uint32_t *widths, *heights, *pitches, *alignments;
    } request = {1,8,0,0,864,478,0x4e563132,0,2,widths,heights,pitches,alignments};
    assert(ops->querySurface(session, &request) == 5);
    request.kind = 1;
    assert(!ops->querySurface(session, &request));
    assert(widths[0] == 864 && widths[1] == 432 && heights[0] == 480 && heights[1] == 240);
    assert(pitches[0] == 1024 && pitches[1] == 1024 && alignments[0] == 256 && alignments[1] == 256);
    request.op = 16;
    request.fn = 1;
    request.kind = 0;
    assert(!ops->querySurface(session, &request));
    assert(widths[1] == 432 && pitches[1] == 1024);
}

typedef int (*CreateAccelerator)(void **, const void *, void *);
typedef int (*DestroyAccelerator)(void *);

#if !PHX_TEST_BUNDLE
static void checkHDContexts(Interface *ops, void *session, Caps *caps, const Function *dither) {
    Context output = {.operation = 16, .function = 1, .engine = 4, .w = 1920, .h = 1088};
    Context decode = {.operation = 1, .function = 8, .engine = 4, .w = 1920, .h = 1088, .related = &output};
    Context post = {.operation = dither->op, .function = dither->fn, .engine = dither->engine, .w = 1920, .h = 1080};
    void *inputs[] = {&post, &decode}, *handles[] = {session, session};
    ContextArray list = {2, inputs, handles};
    ContextRequest request = {&list, caps};
    unsigned before = openAttempts;
    post.h = 1072;
    assert(ops->createContexts(session, &request) && !handles[0] && !handles[1] && !request.capabilities);
    assert(!openDecoder && openAttempts == before);
    post.h = 1080;
    assert(ops->createContexts(session, &request) && !handles[0] && !handles[1] && !request.capabilities);
    assert(!openDecoder && openAttempts == before + 1); // Build 68 rejects coded 1920x1088 at open.
    allowHD = true;
    assert(!ops->createContexts(session, &request) && handles[0] && handles[1] && request.capabilities == caps);
    assert(openDecoder && openAttempts == before + 2);

    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *key = kCVPixelBufferIOSurfacePropertiesKey, *value = empty;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef source = NULL, destination = NULL;
    assert(!CVPixelBufferCreate(NULL, 1920, 1088, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &source));
    assert(!CVPixelBufferCreate(NULL, 1920, 1080, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &destination));
    assert(!CVPixelBufferLockBaseAddress(source, 0));
    for (size_t i = 0; i < 2; ++i) {
        uint8_t *plane = CVPixelBufferGetBaseAddressOfPlane(source, i);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(source, i);
        for (size_t row = 0; row < (1088u >> i); ++row)
            memset(plane + row * stride, (int)(row + i), 1920);
    }
    assert(!CVPixelBufferUnlockBaseAddress(source, 0));
    PhoenixAVDDitherPicture picture = {.source = CVPixelBufferGetIOSurface(source),
                                       .destination = CVPixelBufferGetIOSurface(destination)};
    PhoenixAVDCommand command = {.operation = post.operation, .function = post.function, .commandID = 51,
                                 .context = handles[0], .picture = &picture};
    void *cmd = &command, *completion = NULL;
    Array commands = {1, &cmd}, results = {1, &completion};
    assert(!ops->submit(session, &commands, &results) && completion);
    WaitRecord record = {.completion = completion, .commandID = command.commandID, .blocking = 1};
    Wait wait = {1, &record};
    assert(!ops->wait(session, &wait) && record.status == 1 && !record.error);
    assert(!CVPixelBufferLockBaseAddress(destination, kCVPixelBufferLock_ReadOnly));
    for (size_t i = 0; i < 2; ++i) {
        const uint8_t *plane = CVPixelBufferGetBaseAddressOfPlane(destination, i);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(destination, i);
        for (size_t row = 0; row < (1080u >> i); ++row)
            for (size_t x = 0; x < 1920; ++x) assert(plane[row * stride + x] == (uint8_t)(row + i));
    }
    assert(!CVPixelBufferUnlockBaseAddress(destination, kCVPixelBufferLock_ReadOnly));
    CFRelease(source);
    CFRelease(destination);
    CFRelease(attrs);
    CFRelease(empty);
    Array both = {2, handles};
    assert(!ops->destroyContexts(session, &both) && !openDecoder);
    list.count = 1;
    assert(!ops->createContexts(session, &request) && handles[0]);
    list.inputs = &inputs[1];
    list.outputs = &handles[1];
    decode.w = 864;
    decode.h = 480;
    assert(ops->createContexts(session, &request) && !handles[1] && !request.capabilities);
    assert(openAttempts == before + 2);
    Array onlyPost = {1, handles};
    assert(!ops->destroyContexts(session, &onlyPost));
    allowHD = false;
}
#endif

static void checkCallbacks(io_service_t service, bool expectDecode, CreateAccelerator create,
                           DestroyAccelerator destroy) {
    Device device = {.mask = 1, .services = &service};
    union { Renderer renderer; uint8_t storage[0x128]; } description = {0};
    void *accelerator = NULL, *session = NULL;
    assert(!create(&accelerator, &device, &description));
    CFTypeRef published = IORegistryEntryCreateCFProperty(service, CFSTR("IOVARendererID"), NULL, 0);
    int64_t registryID = 0;
    if (published) {
        if (CFGetTypeID(published) == CFNumberGetTypeID() &&
            !CFNumberGetValue((CFNumberRef)published, kCFNumberSInt64Type, &registryID)) registryID = 0;
        CFRelease(published);
    }
    bool knownID = registryID == 0x01020000 || registryID == 0x01080001;
    assert(description.renderer.renderer == (knownID ? (uint32_t)registryID : 0x01020000));
    assert(description.renderer.rendererIDFromRegistry == knownID);
    Interface *ops = accelerator;
    Caps *caps = description.renderer.capabilities;
    Function *encode = findFunction(caps, 32, 0x800);
    Function *decode = findFunction(caps, 1, 8), *out = findFunction(caps, 16, 1);
    Function *dither = findFunction(caps, PhoenixAVDPostprocess, PhoenixAVDDither);
    assert(encode && encode->engine == 4);
    checkEngine(caps, encode->engine);
    if (expectDecode) {
        assert(decode && out && dither && !dither->related);
        assert(decode->related == out && !out->related && decode->engine == 4 && out->engine == 4);
        checkEngine(caps, dither->engine);
    } else {
        assert(caps->count == 1 && caps->engines == 1 && !decode && !out && !dither);
    }
    SessionSettings settings = {.accelerator = ops->privateData};
    assert(!ops->create(&session, &settings));

    uint32_t encodeBW[] = {32, 0x800, 4, 0}, decodeBW[] = {1, 8, 4, 0}, outputBW[] = {16, 1, 4, 0};
    Property items[] = {{9, encodeBW}, {9, decodeBW}, {9, outputBW}};
    Properties properties = {3, items};
    assert(ops->getProperties(session, &properties) == (expectDecode ? 0 : 1));
    assert((encodeBW[3] != 0) == expectDecode); // Failed multi-property query did not write the first entry.

    void *sentinel = session;
    Property capsItems[] = {{14, &sentinel}, {9, encodeBW}};
    Properties capsQuery = {2, capsItems};
    assert(!ops->getProperties(session, &capsQuery));
    assert(capsItems[0].value == caps && sentinel == session && encodeBW[3]);
    capsItems[0].value = NULL;
    capsItems[1].id = UINT32_MAX;
    assert(ops->getProperties(session, &capsQuery) && !capsItems[0].value);
    capsQuery.count = 1;
    assert(!ops->getProperties(session, &capsQuery) && capsItems[0].value == caps);

    Context output = {.operation = 16, .function = 1, .engine = 4, .w = 864, .h = 480};
    Context context = {.operation = 1, .function = 8, .engine = 4, .w = 864, .h = 480};
    void *input = &context, *active = NULL;
    ContextArray list = {1, &input, &active};
    ContextRequest request = {&list, NULL};
    assert(ops->createContexts(session, &request) && !active && !openDecoder);
    context.related = &output;
    if (!expectDecode) {
        assert(ops->createContexts(session, &request) && !active && !openDecoder);
        goto done;
    }
    uint32_t postBW[] = {dither->op, (uint32_t)dither->fn, dither->engine, 0};
    Property postItem = {9, postBW};
    Properties postQuery = {1, &postItem};
    assert(!ops->getProperties(session, &postQuery) && postBW[3] != 0);
    checkSurface(ops, session);
#if !PHX_TEST_BUNDLE
    checkHDContexts(ops, session, caps, dither);
#endif
    assert(decodeBW[3] && outputBW[3] && encodeBW[3]);
    context.h = 1080;
    assert(ops->createContexts(session, &request) && !active && !openDecoder);
    context.h = 480;
    Context post = {.operation = dither->op, .function = dither->fn, .engine = dither->engine,
                    .w = 864, .h = 480};
    void *inputs[] = {&context, &post}, *handles[] = {session, session};
    ContextArray mixedList = {2, inputs, handles};
    ContextRequest mixed = {&mixedList, caps};
    post.function = 0;
    assert(ops->createContexts(session, &mixed) && !mixed.capabilities &&
           !handles[0] && !handles[1] && !openDecoder);
    post.function = dither->fn;
    assert(!ops->createContexts(session, &mixed) && mixed.capabilities == caps);
    active = handles[0];
    void *postHandle = handles[1];
    assert(active && postHandle && active != postHandle && (PHX_TEST_BUNDLE || openDecoder));

    enum { offset = 4080, capacity = offset + sizeof(fixture) };
    uint8_t *mapped[2] = {0};
    Pool pool = {.id = 0, .bytes = capacity, .count = 2, .mappings = mapped};
    assert(!ops->allocatePool(session, &pool) && mapped[0] != mapped[1] && !((uintptr_t)mapped[1] % 128));
    memcpy(mapped[1] + offset, fixture, sizeof(fixture));
    uint32_t sps[0xc18/4] = {[0]=1,[1]=100,[9]=30,[11]=1,[257]=1,[259]=3,[520]=2,
                             [522]=53,[523]=29,[524]=1,[526]=1};
    uint32_t pps[0x488/4] = {[0]=1,[4]=1,[40]=2,[43]=1};
    const uint32_t lengths[] = {277,279,280,280,280};
    PhoenixAVDSlice slices[5];
    const PhoenixAVDSlice *pointers[5];
    uint32_t pos = offset;
    for (unsigned i = 0; i < 5; ++i) {
        slices[i] = (PhoenixAVDSlice){lengths[i], pos, 0};
        pointers[i] = &slices[i];
        pos += lengths[i];
    }
    assert(pos == capacity);
    PhoenixAVDDecodeTarget target = {.surface = surface};
    PhoenixAVDCommand targetCommand = {.operation = 16, .function = 1, .context = active, .picture = &target};
    PhoenixAVDDecodePicture picture = {.sps = sps, .pps = pps, .bufferIndex = 1, .sliceCount = 5, .slices = pointers};
    PhoenixAVDCommand command = {.operation = 1, .function = 8, .commandID = 3, .context = active,
                                 .picture = &picture, .next = &targetCommand};
    void *cmd = &command, *result = NULL;
    Array commands = {1, &cmd}, results = {1, &result};
#define INVALID(field, value, restore) do { field = value; result = active; \
    assert(ops->submit(session, &commands, &results) && !result && calls == 0); field = restore; } while (0)
    INVALID(command.next, NULL, &targetCommand);
    INVALID(targetCommand.operation, 32, 16);
    INVALID(targetCommand.context, NULL, active);
    INVALID(targetCommand.context, postHandle, active);
    INVALID(command.context, postHandle, active);
    INVALID(target.surface, NULL, surface);
    INVALID(picture.poolID, 2, 0);
    INVALID(picture.bufferIndex, 2, 1);
    INVALID(picture.sliceCount, 6, 5);
    INVALID(pointers[2], NULL, &slices[2]);
    INVALID(slices[1].offset, offset, offset + lengths[0]);
    INVALID(slices[4].length, UINT32_MAX, lengths[4]);
    INVALID(slices[0].length, 0, lengths[0]);
    INVALID(sps[11], 3, 1);
    INVALID(pps[4], 0, 1);
#undef INVALID
    PhoenixAVDDitherPicture passthrough = {.source = surface, .destination = surface};
    PhoenixAVDCommand postCommand = {.operation = dither->op, .function = dither->fn, .commandID = 4,
                                     .context = active, .picture = &passthrough};
    void *postInput = &postCommand, *postResult = NULL;
    Array postCommands = {1, &postInput}, postResults = {1, &postResult};
    assert(ops->submit(session, &postCommands, &postResults) && !postResult);
    postCommand.context = postHandle;
    passthrough.enabled[0] = 1;
    assert(ops->submit(session, &postCommands, &postResults) && !postResult);
    passthrough.enabled[0] = 0;
    if (!PHX_TEST_BUNDLE) {
        failFrame = true;
        assert(ops->submit(session, &commands, &results) && !result && calls == 1);
        failFrame = false;
    }
    void *mixedInputs[] = {&command, &postCommand}, *mixedResults[] = {NULL, NULL};
    Array batch = {2, mixedInputs}, completions = {2, mixedResults};
    assert(!ops->submit(session, &batch, &completions) && mixedResults[0] && mixedResults[1]);
    assert(mixedResults[0] != mixedResults[1] && (PHX_TEST_BUNDLE || calls == 2));
    WaitRecord records[] = {{.completion = mixedResults[0], .commandID = command.commandID, .blocking = 1},
                            {.completion = mixedResults[1], .commandID = postCommand.commandID, .blocking = 1}};
    Wait wait = {2, records};
    assert(!ops->wait(session, &wait) && records[0].status == 1 && !records[0].error &&
           records[1].status == 1 && !records[1].error);
    void *duplicates[] = {active, active};
    Array duplicateContexts = {2, duplicates};
    assert(ops->destroyContexts(session, &duplicateContexts) && (PHX_TEST_BUNDLE || openDecoder));
    assert(!ops->wait(session, &wait));

    Array postContexts = {1, &postHandle};
    assert(!ops->destroyContexts(session, &postContexts));
    Wait postWait = {1, &records[1]};
    assert(ops->wait(session, &postWait) && records[1].status == 3 && records[1].error);
    Wait decodeWait = {1, &records[0]};
    assert(!ops->wait(session, &decodeWait) && records[0].status == 1 && !records[0].error);
    void *postInfo = &post;
    ContextArray postList = {1, &postInfo, &handles[1]};
    ContextRequest postRequest = {&postList, NULL};
    assert(!ops->createContexts(session, &postRequest) && handles[1] && handles[1] != active);
    assert(ops->wait(session, &postWait));

    assert(!ops->releasePool(session, pool.id));
    Array contexts = {2, handles};
    assert(!ops->destroyContexts(session, &contexts) && !openDecoder);
    assert(ops->wait(session, &wait));
done:
    assert(!ops->destroy(session) && !destroy(accelerator));
}

#if PHX_TEST_BUNDLE
static bool matchesReference(CVPixelBufferRef pixel, const char *path) {
    FILE *reference = fopen(path, "rb");
    if (!reference) { perror(path); return false; }
    assert(!CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly));
    size_t checked = 0, mismatches = 0;
    uint8_t expected[PHX_VIDEO_DECODE_WIDTH];
    for (unsigned plane = 0; plane < 2; ++plane) {
        const uint8_t *row = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        size_t rows = CVPixelBufferGetHeightOfPlane(pixel, plane);
        assert(row && pitch >= sizeof(expected) && rows == (PHX_VIDEO_DECODE_HEIGHT >> plane));
        for (size_t y = 0; y < rows; ++y, row += pitch) {
            if (fread(expected, 1, sizeof(expected), reference) != sizeof(expected)) break;
            for (size_t x = 0; x < sizeof(expected); ++x) mismatches += expected[x] != row[x];
            checked += sizeof(expected);
        }
    }
    bool valid = fgetc(reference) == EOF && !ferror(reference) &&
                 checked == PHX_VIDEO_DECODE_WIDTH * PHX_VIDEO_DECODE_HEIGHT * 3 / 2 && !mismatches;
    assert(!CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly));
    fclose(reference);
    printf("AVD completion received, NV12 visible bytes checked=%zu mismatches=%zu\n", checked, mismatches);
    return valid;
}
#endif

int main(int argc, char **argv) {
#if PHX_TEST_BUNDLE
    assert(argc == 4);
#else
    assert(argc == 3);
    bool probe = !strcmp(argv[2], "probe");
    assert(probe || !strcmp(argv[2], "release"));
#endif
    FILE *f = fopen(argv[1], "rb");
    static const uint8_t parameterSets[] = {0,0,0,1,0x67,0x64,0,0x1e,0xac,0x52,0x30,0x36,0x0f,0x64,
                                             0,0,0,1,0x68,0xde,0x09,0xc8};
    uint8_t header[sizeof(parameterSets)];
    assert(f && fread(header, 1, sizeof(header), f) == sizeof(header) && !memcmp(header, parameterSets, sizeof(header)));
    assert(fread(fixture, 1, sizeof(fixture), f) == sizeof(fixture));
    assert(fgetc(f) == EOF && !ferror(f) && !fclose(f));
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *key = kCVPixelBufferIOSurfacePropertiesKey, *value = empty;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pixel = NULL;
    assert(!CVPixelBufferCreate(NULL, 864, 480, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pixel));
    surface = CVPixelBufferGetIOSurface(pixel);
    assert(surface);
#if PHX_TEST_BUNDLE
    void *bundle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!bundle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    CreateAccelerator create = dlsym(bundle, "AVD_CreateAVDAccelerator");
    DestroyAccelerator destroy = dlsym(bundle, "AVD_DestroyAVDAccelerator");
    assert(create && destroy);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    assert(service);
    checkCallbacks(service, true, create, destroy);
    bool passed = matchesReference(pixel, argv[3]);
    dlclose(bundle);
#else
    io_service_t service = IORegistryGetRootEntry(kIOMainPortDefault);
    assert(service);
    mockService = service;
    checkRenderer(NULL, 0x01020000, false);
    checkRenderer(CFSTR("0x01080001"), 0x01020000, false);
    const int64_t ids[] = {0x01020000, 0x01080001, 0x01080002, 0x101080001};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        CFNumberRef id = CFNumberCreate(NULL, kCFNumberSInt64Type, &ids[i]);
        checkRenderer(id, i < 2 ? (uint32_t)ids[i] : 0x01020000, i < 2);
        CFRelease(id);
    }
    double fractional = 0x01080001 + 0.5;
    CFNumberRef id = CFNumberCreate(NULL, kCFNumberDoubleType, &fractional);
    checkRenderer(id, 0x01020000, false);
    CFRelease(id);
    puts("Renderer selection passed for both registry IDs and missing, mistyped, unknown, wide and fractional values");
    checkCallbacks(service, false, PhoenixAVDCreate, PhoenixAVDDestroy);
    available = true;
    checkCallbacks(service, probe, PhoenixAVDCreate, PhoenixAVDDestroy);
#endif
    IOObjectRelease(service);
    CFRelease(pixel);
    CFRelease(attrs);
    CFRelease(empty);
#if PHX_TEST_BUNDLE
    return passed ? 0 : 1;
#else
    puts(probe ? "Reference probe passed: 864x480 callbacks, 1920x1088 to 1080 crop, rejected batch rollback (simulated decoder)"
               : "Release passed: encoder only, including when the reference decode ABI is present");
    return 0;
#endif
}
