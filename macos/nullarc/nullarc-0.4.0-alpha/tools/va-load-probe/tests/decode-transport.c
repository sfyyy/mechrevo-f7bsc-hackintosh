// Direct IOAccel transport regression. Run with run-decode-transport.sh.
#include "phoenix_decode.h"
#include "PhxVideoDecode.h"
#include <VideoToolbox/VTErrors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Width = PHX_VIDEO_DECODE_WIDTH, Height = PHX_VIDEO_DECODE_HEIGHT,
       Slices = PHX_VIDEO_DECODE_INPUT_BYTES, Visible = Width * Height * 3 / 2 };
static const uint8_t parameters[] = {0,0,0,1,0x67,0x64,0,0x1e,0xac,0x52,0x30,0x36,0x0f,0x64,
                                      0,0,0,1,0x68,0xde,0x09,0xc8};
static uint8_t stream[sizeof(parameters) + Slices], expected[Visible];
static const uint8_t *const slices = stream + sizeof(parameters);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return false; } } while (0)

#if PHX_TEST_MOCK
#include "phoenix_video_context.h"
static unsigned calls;
static bool shortReply, offline;
static int streamVersion;
static const uint8_t *streamAU, *streamNV12;
static size_t auBytes, nv12Bytes;
Device IOAccelDeviceCreate(io_service_t service) { return service == 1 ? (Device)1 : NULL; }
Shared IOAccelSharedCreate(Device d) { return d ? (Shared)1 : NULL; }
Video IOAccelVideoContextCreate(Shared s) { return s ? (Video)1 : NULL; }
io_connect_t IOAccelVideoContextGetConnect(Video v) { return v ? 2 : 0; }
void IOAccelDeviceRelease(Device d) { (void)d; }
void IOAccelSharedRelease(Shared s) { (void)s; }
void IOAccelVideoContextRelease(Video v) { (void)v; }
kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t service, uint64_t *entryID) {
    if (service != 1) return kIOReturnNoDevice;
    *entryID = 0;
    return KERN_SUCCESS;
}
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t service, CFStringRef key,
                                          CFAllocatorRef allocator, IOOptionBits options) {
    (void)options;
    const int version = PHX_VIDEO_DECODE_VERSION;
    if (service == 1 && CFEqual(key, CFSTR(PHX_VIDEO_DECODE_PROPERTY)))
        return CFNumberCreate(allocator, kCFNumberIntType, &version);
    if (service == 1 && streamVersion && CFEqual(key, CFSTR(PHX_VIDEO_DECODE_STREAM_PROPERTY)))
        return CFNumberCreate(allocator, kCFNumberIntType, &streamVersion);
    return NULL;
}
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *input,
                                        size_t inputSize, void *output, size_t *outputSize) {
    ++calls;
    if (selector == PHX_VIDEO_DECODE_STREAM_AVC) {
        if (connection != 2 || !streamAU || !streamNV12 || inputSize != auBytes ||
            !output || !outputSize || *outputSize != nv12Bytes || memcmp(streamAU, input, auBytes))
            return kIOReturnBadArgument;
        memcpy(output, streamNV12, nv12Bytes);
        if (shortReply) --*outputSize;
        return KERN_SUCCESS;
    }
    if (connection != 2 || selector != PHX_VIDEO_DECODE_AVC || inputSize != Slices ||
        !output || !outputSize || *outputSize != PHX_VIDEO_DECODE_OUTPUT_BYTES) return kIOReturnBadArgument;
    if (memcmp(input, slices, Slices)) return kIOReturnUnsupported;
    if (offline) return kIOReturnNotReady;
    memset(output, 0x3a, *outputSize);
    for (size_t row = 0; row < Height * 3 / 2; ++row)
        memcpy((uint8_t *)output + row * PHX_VIDEO_DECODE_PITCH, expected + row * Width, Width);
    if (shortReply) --*outputSize;
    return KERN_SUCCESS;
}
#endif

static bool readExact(const char *path, void *buffer, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    bool ok = fread(buffer, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    return !fclose(f) && ok;
}

static CVPixelBufferRef makeSizedPixel(size_t width, size_t height) {
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const int alignment = 2048;
    CFNumberRef align = CFNumberCreate(NULL, kCFNumberIntType, &alignment);
    CVPixelBufferRef image = NULL;
    if (attrs && align && surface) {
        CFDictionarySetValue(attrs, kCVPixelBufferBytesPerRowAlignmentKey, align);
        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, surface);
        CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &image);
    }
    if (attrs) CFRelease(attrs);
    if (surface) CFRelease(surface);
    if (align) CFRelease(align);
    return image;
}

enum PixelCheck { Initialize, Untouched, Decoded };
static bool checkPixel(CVPixelBufferRef image, enum PixelCheck check) {
    CHECK(!CVPixelBufferLockBaseAddress(image, 0));
    bool ok = true;
    size_t offset = 0;
    for (unsigned plane = 0; plane < 2; ++plane) {
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
        size_t rows = CVPixelBufferGetHeightOfPlane(image, plane);
        uint8_t *row = CVPixelBufferGetBaseAddressOfPlane(image, plane);
        if (!row || pitch <= Width || rows != (Height >> plane)) { ok = false; break; }
        for (size_t y = 0; y < rows; ++y, row += pitch, offset += Width) {
            if (check == Initialize) { memset(row, 0xc5, pitch); continue; }
            if (check == Decoded && memcmp(row, expected + offset, Width)) ok = false;
            for (size_t col = check == Decoded ? Width : 0; col < pitch; ++col)
                if (row[col] != 0xc5) { ok = false; break; }
        }
    }
    CHECK(!CVPixelBufferUnlockBaseAddress(image, 0));
    return ok;
}

static bool run(PhoenixVADecoder *decoder, CVPixelBufferRef image) {
    CHECK(checkPixel(image, Initialize));
    uint8_t changed[Slices];
    memcpy(changed, slices, Slices);
    changed[Slices - 1] ^= 1;
    CHECK(PhoenixVADecodeFrame(decoder, changed, sizeof(changed), image) == kVTVideoDecoderUnsupportedDataFormatErr);
    CHECK(checkPixel(image, Untouched));
    CHECK(PhoenixVADecodeFrame(decoder, slices, 1, image) == kVTVideoDecoderUnsupportedDataFormatErr);
#if PHX_TEST_MOCK
    CHECK(calls == 1); // Only the structurally valid mutation reached IOConnect.
    shortReply = true;
    CHECK(PhoenixVADecodeFrame(decoder, slices, Slices, image) == kVTVideoDecoderMalfunctionErr);
    shortReply = false;
    CHECK(checkPixel(image, Untouched));
    offline = true;
    CHECK(PhoenixVADecodeFrame(decoder, slices, Slices, image) == kVTVideoDecoderNotAvailableNowErr);
    offline = false;
    CHECK(checkPixel(image, Untouched));
#endif
    // Test both legal prefix lengths, comparing all visible rows and their padding.
    uint8_t fourByte[Slices + 5];
    size_t used = 0;
    for (size_t i = 0; i < Slices; ++i) {
        if (i + 2 < Slices && slices[i] == 0 && slices[i+1] == 0 && slices[i+2] == 1) {
            CHECK(used < sizeof(fourByte));
            fourByte[used++] = 0;
        }
        CHECK(used < sizeof(fourByte));
        fourByte[used++] = slices[i];
    }
    CHECK(used == sizeof(fourByte));
    OSStatus status = PhoenixVADecodeFrame(decoder, slices, Slices, image);
    if (status) fprintf(stderr, "three-byte-prefix frame status=%d\n", (int)status);
    CHECK(!status && checkPixel(image, Decoded));
    CHECK(checkPixel(image, Initialize));
    status = PhoenixVADecodeFrame(decoder, fourByte, used, image);
    if (status) fprintf(stderr, "four-byte-prefix frame status=%d\n", (int)status);
    CHECK(!status && checkPixel(image, Decoded));
    return true;
}

#if PHX_TEST_MOCK
static bool checkStreamPixel(CVPixelBufferRef pixel, enum PixelCheck mode) {
    CHECK(!CVPixelBufferLockBaseAddress(pixel, 0));
    size_t offset = 0;
    bool match = true;
    for (unsigned plane = 0; plane < 2; ++plane) {
        uint8_t *row = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        size_t rows = CVPixelBufferGetHeightOfPlane(pixel, plane);
        CHECK(row && pitch > 1920);
        for (size_t y = 0; y < rows; ++y, row += pitch) {
            if (mode == Initialize) { memset(row, 0x5c, pitch); continue; }
            size_t filled = 0;
            if (mode == Decoded) {
                filled = 1920;
                if (y < (1080u >> plane)) {
                    if (memcmp(row, streamNV12 + offset, filled)) match = false;
                    offset += filled;
                } else {
                    for (size_t x = 0; x < filled; ++x)
                        if (row[x] != (plane ? 128 : 16)) match = false;
                }
            }
            for (size_t x = filled; x < pitch; ++x)
                if (row[x] != 0x5c) match = false;
        }
    }
    CHECK(!CVPixelBufferUnlockBaseAddress(pixel, 0));
    return match;
}

static bool runStream(const char *directory) {
    PhoenixVADecoder *decoder = NULL;
    CMVideoDimensions coded = {1920, 1088}, visible = {1920, 1080};
    CHECK(!PhoenixVADecodeStreamAvailable(1));
    CHECK(PhoenixVADecodeOpen(1, coded, &decoder) == kVTVideoDecoderUnsupportedDataFormatErr && !decoder);
    streamVersion = 2;
    CHECK(!PhoenixVADecodeStreamAvailable(1));
    streamVersion = PHX_VIDEO_DECODE_STREAM_VERSION;
    CHECK(PhoenixVADecodeStreamAvailable(1) && !PhoenixVADecodeStreamAvailable(0));
    CHECK(!PhoenixVADecodeOpen(1, coded, &decoder) && PhoenixVADecodeHasStream(decoder));
    CVPixelBufferRef images[2] = {makeSizedPixel(1920, 1080), makeSizedPixel(1920, 1088)};
    CHECK(images[0] && images[1]);
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/reference.nv12", directory) < (int)sizeof(path));
    FILE *raw = fopen(path, "rb");
    CHECK(raw);
    nv12Bytes = 1920u * 1080u * 3u / 2u;
    uint8_t *packed = malloc(nv12Bytes);
    uint8_t *input = malloc(PHX_VIDEO_DECODE_STREAM_MAX_INPUT);
    CHECK(packed && input);
    streamNV12 = packed;
    streamAU = input;
    unsigned frame = 0;
    size_t got;
    while ((got = fread(packed, 1, nv12Bytes, raw)) != 0) {
        CHECK(got == nv12Bytes);
        CHECK(snprintf(path, sizeof(path), "%s/frame-%04u.au", directory, frame) < (int)sizeof(path));
        FILE *au = fopen(path, "rb");
        CHECK(au);
        auBytes = fread(input, 1, PHX_VIDEO_DECODE_STREAM_MAX_INPUT, au);
        CHECK(auBytes && !ferror(au) && fgetc(au) == EOF && !fclose(au));
        CVPixelBufferRef image = images[frame % 2];
        CHECK(checkStreamPixel(image, Initialize));
        if (!frame) {
            CHECK(PhoenixVADecodeAccessUnit(decoder, input, auBytes, (CMVideoDimensions){1920, 1040}, image)
                  == kVTVideoDecoderUnsupportedDataFormatErr && !calls);
            shortReply = true;
            CHECK(PhoenixVADecodeAccessUnit(decoder, input, auBytes, visible, image) == kVTVideoDecoderMalfunctionErr);
            shortReply = false;
            CHECK(checkStreamPixel(image, Untouched));
        }
        CHECK(!PhoenixVADecodeAccessUnit(decoder, input, auBytes, visible, image));
        CHECK(checkStreamPixel(image, Decoded));
        ++frame;
    }
    CHECK(frame == 14 && calls == 15 && !ferror(raw) && !fclose(raw));
    free(input);
    free(packed);
    CFRelease(images[0]);
    CFRelease(images[1]);
    PhoenixVADecodeClose(decoder);
    puts("14 simulated stream submissions matched packed 1080p NV12, coded padding and row strides");
    return true;
}
#endif

int main(int argc, char **argv) {
#if PHX_TEST_MOCK
    if (argc == 2) return runStream(argv[1]) ? 0 : 1;
#endif
    if (argc != 3 || !readExact(argv[1], stream, sizeof(stream)) || memcmp(stream, parameters, sizeof(parameters)) ||
        !readExact(argv[2], expected, sizeof(expected))) {
        fputs("expected reference Annex B access unit and tightly packed VCN NV12 reference\n", stderr);
        return 2;
    }
#if PHX_TEST_MOCK
    io_service_t service = 1;
    const char *mode = "simulated IOKit transport";
#else
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    const char *mode = "real IOAccel video context";
#endif
    PhoenixVADecoder *decoder = NULL;
    OSStatus status = PhoenixVADecodeOpen(service, (CMVideoDimensions){Width, Height}, &decoder);
#if !PHX_TEST_MOCK
    if (service) IOObjectRelease(service);
#endif
    if (status) {
        fprintf(stderr, "%s: open failed, OSStatus=%d (requires %s=%u)\n", mode, (int)status,
                PHX_VIDEO_DECODE_PROPERTY, PHX_VIDEO_DECODE_VERSION);
        return 1;
    }
    CVPixelBufferRef image = makeSizedPixel(Width, Height);
    bool ok = image && run(decoder, image);
    if (image) CFRelease(image);
    PhoenixVADecodeClose(decoder);
    if (ok) printf("%s: two reference submissions passed, each matched %u visible NV12 bytes; row padding preserved\n", mode, Visible);
    return ok ? 0 : 1;
}
