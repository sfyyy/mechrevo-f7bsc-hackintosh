// Adapter for stream-callback.py. Verify the exact AU the AVD callback sends.
#include "avd_decode.h"
#include <string.h>

struct PhoenixVADecoder {
    const uint8_t *expected;
    size_t bytes;
    CMVideoDimensions visible;
    unsigned calls;
    OSStatus result;
};

bool PhoenixVADecodeHasStream(const PhoenixVADecoder *decoder) { return decoder != NULL; }
bool PhoenixVADecodeHasReference(const PhoenixVADecoder *decoder) { (void)decoder; return false; }
OSStatus PhoenixVADecodeFrame(PhoenixVADecoder *decoder, const uint8_t *data, size_t n, CVPixelBufferRef pixel) {
    (void)decoder; (void)data; (void)n; (void)pixel;
    return -1;
}

OSStatus PhoenixVADecodeAccessUnit(PhoenixVADecoder *decoder, const uint8_t *data, size_t bytes,
                                   CMVideoDimensions visible, CVPixelBufferRef pixel) {
    ++decoder->calls;
    return !pixel || bytes != decoder->bytes || memcmp(data, decoder->expected, bytes) ||
           visible.width != decoder->visible.width || visible.height != decoder->visible.height ? -1 : decoder->result;
}

int checkCallback(const uint32_t *sps, const uint32_t *pps, const uint8_t *au, size_t bytes,
                  const uint32_t *lengths, unsigned count, uint32_t offset, int32_t width, int32_t height) {
    if (!count || count > 32 || offset >= bytes || bytes > UINT32_MAX) return -1;
    CVPixelBufferRef pixel = NULL;
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *key = kCVPixelBufferIOSurfacePropertiesKey, *value = empty;
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVReturn created = CVPixelBufferCreate(NULL, width, (height+15)&~15,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &pixel);
    CFRelease(attributes);
    CFRelease(empty);
    if (created || !pixel) return -1;
    PhoenixAVDSlice slices[32];
    const PhoenixAVDSlice *pointers[32];
    for (unsigned i = 0; i < count; ++i) {
        slices[i] = (PhoenixAVDSlice){lengths[i], offset, 0};
        pointers[i] = &slices[i];
        offset += lengths[i];
    }
    PhoenixVADecoder decoder = {.expected=au, .bytes=bytes, .visible={width,height}};
    PhoenixAVDDecodeTarget target = {.surface=CVPixelBufferGetIOSurface(pixel)};
    PhoenixAVDCommand output = {.operation=PhoenixAVDOutput, .function=PhoenixAVDOutputNV12,
                                .context=&decoder, .picture=&target};
    PhoenixAVDDecodePicture picture = {.sps=sps, .pps=pps, .sliceCount=count, .slices=pointers};
    PhoenixAVDCommand command = {.operation=PhoenixAVDDecode, .function=PhoenixAVDDecodeAVC,
                                 .context=&decoder, .picture=&picture, .next=&output};
    uint8_t *mapping = (uint8_t *)au;
    PhoenixAVDMappedPool pool = {.bytesPerBuffer=(uint32_t)bytes, .count=1, .buffers=&mapping};
    uint32_t reportedRejections = 0;
    int status = PhoenixAVDDecodeAVCFrame(&decoder, &reportedRejections, &command, &pool, 1);
    if (!status && decoder.calls == 1 && !reportedRejections) {
        // Exercise the same valid AU with a transport rejection, twice. Each
        // kind retains just one bit in the per-session report mask.
        unsigned nal = au[slices[0].offset + 4] & 31;
        uint32_t expectedMask = 1u << (14u + (nal == 5 ? 5u : 0u));
        decoder.result = -1;
        for (unsigned i = 0; i < 2; ++i) {
            if (!PhoenixAVDDecodeAVCFrame(&decoder, &reportedRejections, &command, &pool, 1) ||
                reportedRejections != expectedMask) status = -1;
        }
        ++slices[count - 1].length;
        if (!PhoenixAVDDecodeAVCFrame(&decoder, &reportedRejections, &command, &pool, 1) ||
            decoder.calls != 3) status = -1;
    } else status = -1;
    CFRelease(pixel);
    return status;
}
