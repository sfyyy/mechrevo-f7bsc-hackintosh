// SPDX-License-Identifier: MIT
#include "../../avc/avc_stream_parser.h"
#include "../../../src/phxbringup/PhxNativeABI.h"
#include <IOKit/IOKitLib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

struct NativeClient {
    io_connect_t connection = IO_OBJECT_NULL;

    ~NativeClient() {
        if (connection) IOServiceClose(connection);
    }

    bool open() {
        if (geteuid() != 0) {
            std::fprintf(stderr, "Hardware mode requires root for debug selector 121\n");
            return false;
        }
        auto service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
        if (!service) {
            std::fprintf(stderr, "PhxBringup service unavailable\n");
            return false;
        }
        auto result = IOServiceOpen(service, mach_task_self(), PHXN_UC_TYPE, &connection);
        IOObjectRelease(service);
        const uint64_t query[] = {PHXN_ABI_VERSION, 0};
        uint64_t hello[4] = {};
        uint32_t count = 4;
        if (result == KERN_SUCCESS)
            result = IOConnectCallScalarMethod(connection, PHXN_SEL_HELLO, query, 2, hello, &count);
        if (result != KERN_SUCCESS || count != 4 || hello[0] != PHXN_ABI_VERSION) {
            std::fprintf(stderr, "Native open or hello failed, IOReturn=0x%08x, count=%u\n", result, count);
            return false;
        }
        std::fprintf(stderr, "PhxBringup build %llu\n", hello[1]);
        if (hello[1] < 69) {
            std::fprintf(stderr, "Debug selector 121 requires build 69 or later\n");
            return false;
        }
        return true;
    }
};

static bool check(const fs::path &dir, bool hardware) {
    std::ifstream reference(dir / "reference.nv12", std::ios::binary);
    if (!reference) {
        std::fprintf(stderr, "Missing %s/reference.nv12\n", dir.c_str());
        return false;
    }
    NativeClient client;
    if (hardware && !client.open()) return false;

    phx_vcn::AvcStreamParser parser;
    unsigned frame = 0, idrs = 0, predicted = 0;
    while (reference.peek() != EOF) {
        char filename[64];
        std::snprintf(filename, sizeof(filename), "frame-%04u.au", frame);
        std::ifstream input(dir / filename, std::ios::binary | std::ios::ate);
        const std::streamoff length = input.tellg();
        if (length <= 0 || length > phx_vcn::kAvcStreamMaxBitstreamBytes) {
            std::fprintf(stderr, "%s missing or invalid AU length\n", filename);
            return false;
        }
        std::vector<uint8_t> au(static_cast<size_t>(length));
        input.seekg(0);
        phx_vcn::AvcStreamSequence sequence{};
        phx_vcn::AvcStreamPicture picture{};
        uint32_t slices = 0;
        if (!input.read(reinterpret_cast<char *>(au.data()), length) ||
            !parser.parse(au.data(), au.size(), sequence, picture, slices)) {
            std::fprintf(stderr, "%s failed Annex B admission\n", filename);
            return false;
        }
        const size_t visible = static_cast<size_t>(sequence.visibleWidth) * sequence.visibleHeight * 3 / 2;
        std::vector<uint8_t> expected(visible);
        if (!reference.read(reinterpret_cast<char *>(expected.data()), visible)) {
            std::fprintf(stderr, "%s incomplete software reference\n", filename);
            return false;
        }
        if (hardware) {
            std::vector<uint8_t> pixels(visible, 0xa5);
            size_t outputBytes = pixels.size();
            auto result = IOConnectCallStructMethod(client.connection, 121, au.data(), au.size(),
                                                    pixels.data(), &outputBytes);
            if (result != KERN_SUCCESS || outputBytes != visible) {
                std::fprintf(stderr, "%s selector 121 IOReturn=0x%08x bytes=%zu expected=%zu\n",
                             filename, result, outputBytes, visible);
                return false;
            }
            auto diff = std::mismatch(expected.begin(), expected.end(), pixels.begin());
            if (diff.first != expected.end()) {
                const size_t offset = static_cast<size_t>(diff.first - expected.begin());
                std::fprintf(stderr, "%s NV12 byte %zu: software=%u VCN=%u\n",
                             filename, offset, *diff.first, *diff.second);
                return false;
            }
        }
        if (picture.idr) ++idrs;
        else ++predicted;
        std::printf("%s %s frame_num=%u POC=%d slices=%u visible=%ux%u bytes=%zu %s\n",
                    filename, picture.idr ? "IDR" : "P", picture.frameNum, picture.topPoc, slices,
                    sequence.visibleWidth, sequence.visibleHeight, visible, hardware ? "matched" : "fixture");
        ++frame;
    }
    size_t files = 0;
    for (const auto &entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".au") ++files;
    if (reference.bad() || files != frame || !idrs || !predicted) {
        std::fprintf(stderr, "Incomplete stream: %zu AUs, %u reference frames, %u IDR, %u P\n",
                     files, frame, idrs, predicted);
        return false;
    }
    std::printf("%s: %u frames, %u IDR and %u P; %s\n", dir.c_str(), frame, idrs, predicted,
                hardware ? "all packed NV12 frames matched" : "host fixtures checked");
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3 || (std::strcmp(argv[1], "--inspect") && std::strcmp(argv[1], "--hardware"))) {
        std::fprintf(stderr, "usage: %s --inspect|--hardware FIXTURE_DIR [FIXTURE_DIR ...]\n", argv[0]);
        return 2;
    }
    for (int i = 2; i < argc; ++i)
        if (!check(argv[i], !std::strcmp(argv[1], "--hardware"))) return 1;
    return 0;
}
