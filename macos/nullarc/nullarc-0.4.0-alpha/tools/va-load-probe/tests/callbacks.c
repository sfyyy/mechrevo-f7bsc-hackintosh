/* Client of the Phoenix bundle's exported factories. Run with run.sh. */
#include "../avd_encode.h"
#include "callbacks_abi.h"
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { width = 160, height = 128, capacity = 28672, poolID = 5 };
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"check failed at line %d: %s\n",__LINE__,#expr); return 1; } } while (0)
#define CALL(expr) do { int rc = (expr); printf("%s = %d\n", #expr, rc); CHECK(!rc); } while (0)

static CVPixelBufferRef makeInput(const char *sourcePath) {
    const void *key = kCVPixelBufferIOSurfacePropertiesKey;
    CFDictionaryRef props = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!props) return NULL;
    const void *value = props;
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(props);
    if (!attrs) return NULL;
    CVPixelBufferRef pixel = NULL;
    CVReturn rc = CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                       attrs, &pixel);
    CFRelease(attrs);
    if (rc) return NULL;
    if (CVPixelBufferLockBaseAddress(pixel, 0)) {
        CFRelease(pixel);
        return NULL;
    }
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
    uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pixel, 1);
    size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
    size_t chromaPitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
    static const uint8_t colors[][3] = {
        {31,72,188}, {67,160,90}, {99,110,135}, {133,92,152}, {178,186,64},
        {205,56,196}, {148,129,170}, {112,169,115}, {78,191,140}, {53,148,76},
    };
    for (unsigned row = 0; row < height; ++row)
        for (unsigned col = 0; col < width; ++col)
            y[row*pitch + col] = colors[(row/64)*5 + col/32][0];
    for (unsigned row = 0; row < height/2; ++row)
        for (unsigned col = 0; col < width; col += 2) {
            const uint8_t *color = colors[(row/32)*5 + col/32];
            uv[row*chromaPitch + col] = color[1];
            uv[row*chromaPitch + col + 1] = color[2];
        }
    FILE *source = fopen(sourcePath, "wbx");
    bool written = source != NULL;
    for (unsigned row = 0; written && row < height; ++row)
        written = fwrite(y + row*pitch, 1, width, source) == width;
    for (unsigned row = 0; written && row < height/2; ++row)
        written = fwrite(uv + row*chromaPitch, 1, width, source) == width;
    if (source && fclose(source)) written = false;
    if (CVPixelBufferUnlockBaseAddress(pixel, 0) || !written) {
        CFRelease(pixel);
        return NULL;
    }
    return pixel;
}

static int checkBufferRequirements(Interface *ops, void *session) {
    uint8_t request[60], expected[sizeof(request)];
    for (size_t i = 0; i < sizeof(request); ++i) request[i] = (uint8_t)(0x80 + i);
    uint32_t operation = 0;
    memcpy(request, &operation, sizeof(operation));
    memcpy(expected, request, sizeof(request));
    CHECK(ops->queryBuffer(session, NULL) == 5);
    CHECK(ops->queryBuffer(NULL, request) == 5);
    CHECK(ops->queryBuffer(session, request) == 1);
    CHECK(memcmp(request, expected, sizeof(request)) == 0);

    operation = 32;
    memcpy(request, &operation, sizeof(operation));
    memcpy(expected, request, sizeof(request));
    const uint32_t words[] = {8, 8, 0, 0, 0, 0};
    memcpy(expected + 28, words, sizeof(words));
    CALL(ops->queryBuffer(session, request));
    CHECK(memcmp(request, expected, sizeof(request)) == 0);

    operation = 1;
    memcpy(request, &operation, sizeof(operation));
    uint32_t kind = 1;
    memcpy(request + 12, &kind, sizeof(kind));
    memcpy(expected, request, sizeof(request));
    CHECK(ops->queryBuffer(session, request) == 1);
    CHECK(memcmp(request, expected, sizeof(request)) == 0);
    kind = 0;
    memcpy(request + 12, &kind, sizeof(kind));
    memcpy(expected, request, sizeof(request));
    const uint32_t decodeWords[] = {64, 128, 0, 0, 0, 128};
    memcpy(expected + 28, decodeWords, sizeof(decodeWords));
    CALL(ops->queryBuffer(session, request));
    CHECK(memcmp(request, expected, sizeof(request)) == 0);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool queryOnly = argc == 3 && !strcmp(argv[2], "--query-buffer");
    if (!queryOnly && argc != 4) {
        fputs("usage: callbacks <Phoenix bundle executable> {--query-buffer | <new .h264> <new .nv12>}\n",stderr);
        return 2;
    }
    void *image = dlopen(argv[1], RTLD_LOCAL | RTLD_NOW);
    if (!image) { fprintf(stderr,"%s\n",dlerror()); return 1; }
    int (*create)(void **, const void *, void *) = dlsym(image, "AVD_CreateAVDAccelerator");
    int (*destroy)(void *) = dlsym(image, "AVD_DestroyAVDAccelerator");
    CHECK(create && destroy);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    CHECK(service);
    Device device = {.mask = 1, .services = &service};
    // The caller reserves 0x128 bytes for the renderer description.
    union { Renderer renderer; uint8_t storage[0x128]; } description = {0};
    void *accelerator = NULL;
    CALL(create(&accelerator, &device, &description));
    CHECK(accelerator && description.renderer.capabilities);
    Interface *ops = accelerator;
    void *session = NULL;
    SessionSettings settings = {.accelerator = ops->privateData};
    CALL(ops->create(&session, &settings));
    CHECK(session);
    CHECK(checkBufferRequirements(ops, session) == 0);
    if (queryOnly) goto releaseSession;
    Context info = {.operation = 32, .function = 0x800, .engine = 4, .w = width, .h = height};
    void *input = &info, *context = NULL;
    ContextArray contextList = {1, &input, &context};
    ContextRequest request = {&contextList, NULL};
    CALL(ops->createContexts(session, &request));
    CHECK(context && request.capabilities);
    uint8_t *mapped = NULL;
    Pool pool = {.id = poolID, .bytes = capacity, .count = 1, .mappings = &mapped};
    CALL(ops->allocatePool(session, &pool));
    CHECK(mapped);

    CVPixelBufferRef pixel = makeInput(argv[3]);
    CHECK(pixel);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel);
    CHECK(surface && IOSurfaceGetWidth(surface) == width && IOSurfaceGetHeight(surface) == height);
    PhoenixAVDAVCStatistics stats = {0};
    const uint32_t tail = 8;
    PhoenixAVDAVCPicture picture = {.ring = {poolID, 0, capacity, capacity-tail, capacity-tail},
                                    .surface = surface, .statistics = &stats};
    PhoenixAVDCommand command = {.operation = 32, .function = 0x800, .commandID = 1,
                                 .context = context, .picture = &picture};
    void *cmd = &command, *completed = NULL;
    Array commands = {1, &cmd}, results = {1, &completed};
    CALL(ops->submit(session, &commands, &results));
    CHECK(completed && stats.encodedBits > tail*8 && stats.encodedBits % 8 == 0);
    WaitRecord record = {.completion = completed, .commandID = command.commandID};
    Wait wait = {1, &record};
    CALL(ops->wait(session, &wait));
    CHECK(record.status == 1 && record.error == 0);
    record.blocking = 1;
    record.status = UINT32_MAX;
    CALL(ops->wait(session, &wait));
    CHECK(record.status == 1 && record.error == 0);
    uint32_t bytes = stats.encodedBits/8;
    CHECK(bytes < capacity && picture.ring.writeOffset == bytes-tail);
    FILE *output = fopen(argv[2], "wbx");
    CHECK(output);
    CHECK(fwrite(mapped+capacity-tail, 1, tail, output) == tail);
    CHECK(fwrite(mapped, 1, bytes-tail, output) == bytes-tail);
    CHECK(fclose(output) == 0);
    printf("pid=%d uid=%d IOSurface=%u bytes=%u write_offset=%u completion=%u\n",
           getpid(),getuid(),IOSurfaceGetID(surface),bytes,picture.ring.writeOffset,record.status);
    CFRelease(pixel);
    CALL(ops->releasePool(session, poolID));
    Array contexts = {1, &context};
    CALL(ops->destroyContexts(session, &contexts));
releaseSession:
    CALL(ops->destroy(session));
    CALL(destroy(accelerator));
    IOObjectRelease(service);
    CHECK(dlclose(image) == 0);
    return 0;
}
