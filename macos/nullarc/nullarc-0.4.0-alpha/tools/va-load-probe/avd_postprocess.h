#ifndef PHOENIX_AVD_POSTPROCESS_H
#define PHOENIX_AVD_POSTPROCESS_H

#include "avd_encode.h"

enum { PhoenixAVDPostprocess = 2, PhoenixAVDDither = 0x10 };

// AppleGVA x86_64's dither command carries three channel enables at +0x1c.
// With dithering disabled, copy visible NV12 pixels, omitting bottom macroblock padding.
typedef struct {
    IOSurfaceRef source, destination;
    uint32_t phase[3];
    uint32_t enabled[3];
} PhoenixAVDDitherPicture;

bool PhoenixAVDCanCropNV12(size_t sourceWidth, size_t sourceHeight, size_t width, size_t height);
int PhoenixAVDDitherNV12(const PhoenixAVDCommand *command);

#endif
