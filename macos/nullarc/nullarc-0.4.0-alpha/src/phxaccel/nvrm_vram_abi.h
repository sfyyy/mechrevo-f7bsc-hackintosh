/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#define NVRM_VRAM_ABI_VERSION 2u
#define NVRM_VRAM_FN_ALLOC    "nvAllocVram"
#define NVRM_VRAM_FN_FREE     "nvFreeVram"

#define NVRM_VRAM_FN_SCANOUT  "nvScanoutInfo"

#define NVRM_VRAM_BAR1_BUDGET (192ull * 1024ull * 1024ull)

struct NVRMVramRequest {
    unsigned int       version;
    unsigned int       flags;
    unsigned long long size;
    unsigned long long actualSize;
    unsigned long long phys;
    void              *kva;
    void              *handle;
    unsigned long long mappedTotal;
    unsigned int width, height, pitch;
    unsigned long long allocationCookie;
};

/* f7bsc patch: double-buffered scan-out on PhxAMDFB (phxfb.cpp, debug.phxfb_dbuf). BACK takes an NVRMVramRequest and
 * answers the surface to copy the next frame into (flags = its index); FLIP with that request shows it, FLIP with NULL
 * abandons it. Anything but success from BACK: copy into nvScanoutInfo's surface as before. */
#define PHX_FN_SCANOUT_BACK   "phxScanoutBack"
#define PHX_FN_SCANOUT_FLIP   "phxScanoutFlip"
