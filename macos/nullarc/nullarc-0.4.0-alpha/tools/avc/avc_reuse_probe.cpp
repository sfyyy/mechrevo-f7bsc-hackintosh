// SPDX-License-Identifier: MIT
// Checks on real streams the property the IDR session reuse depends on: the
// parser hands every IDR an empty reference list with frame_num 0 and slot 0,
// and the message builder accepts it. Then the firmware sees a complete new
// reference state and does not need the close/create pair.
#include "avc_stream_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace phx_vcn;

namespace {

struct Nal {
    size_t start;
    uint8_t type;
    bool firstMbZero;
};

std::vector<uint8_t> readFile(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? static_cast<size_t>(size) : 0);
    const bool ok = !size || std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok ? data : std::vector<uint8_t>{};
}

std::vector<Nal> splitNals(const std::vector<uint8_t> &data) {
    std::vector<Nal> nals;
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
        const size_t start = (i && !data[i - 1]) ? i - 1 : i;
        const uint8_t type = data[i + 3] & 31u;
        const bool firstMbZero = (data[i + 4] & 0x80u) != 0;   // ue(v) == 0 starts with a 1 bit
        nals.push_back({start, type, firstMbZero});
        i += 2;
    }
    return nals;
}

bool isParameterSet(uint8_t type) { return type == 6 || type == 7 || type == 8 || type == 9; }

}   // namespace

int main(int argc, char **argv) {
    int failures = 0;
    for (int a = 1; a < argc; ++a) {
        const std::vector<uint8_t> data = readFile(argv[a]);
        if (data.empty()) {
            std::printf("%s: cannot read\n", argv[a]);
            ++failures;
            continue;
        }
        const std::vector<Nal> nals = splitNals(data);
        std::vector<size_t> boundaries;   // NAL indices of access units: VCL picture starts
        for (size_t i = 0; i < nals.size(); ++i)
            if ((nals[i].type == 1 || nals[i].type == 5) && nals[i].firstMbZero) boundaries.push_back(i);
        if (boundaries.empty()) {
            std::printf("%s: no access units\n", argv[a]);
            ++failures;
            continue;
        }
        AvcStreamParser parser;
        unsigned accessUnits = 0, idrs = 0, idrWithRefs = 0, badIdrFields = 0, messageFailures = 0, rejected = 0;
        uint32_t words[kAvcStreamMessageDw];
        uint8_t scaling[kAvcStreamScalingBytes];
        size_t cursor = 0;   // the parameter sets in front of a picture belong to its access unit
        for (size_t k = 0; k < boundaries.size(); ++k) {
            size_t end = (k + 1 < boundaries.size()) ? nals[boundaries[k + 1]].start : data.size();
            for (size_t j = boundaries[k + 1]; j > 0 && k + 1 < boundaries.size(); --j) {
                if (!isParameterSet(nals[j - 1].type)) break;
                end = nals[j - 1].start;
            }
            const size_t start = cursor;
            cursor = end;
            const size_t length = end > start ? end - start : 0;
            if (!length) {
                ++accessUnits;
                continue;
            }
            AvcStreamSequence sequence{};
            AvcStreamPicture picture{};
            AvcVclRange range{};
            uint32_t slices = 0;
            if (!parser.parse(data.data() + start, length, sequence, picture, slices, &range) || !range.length) {
                ++rejected;
                ++accessUnits;
                continue;
            }
            ++accessUnits;
            if (picture.idr) {
                ++idrs;
                if (picture.referenceCount) ++idrWithRefs;
                if (picture.frameNum || picture.currentSlot) ++badIdrFields;
            } else if (!picture.referenceCount) {
                std::printf("%s: access unit %u is not an IDR but has no references\n", argv[a], accessUnits);
                ++failures;
            }
            if (!buildAvcStreamMessage(words, scaling, sequence, picture, 1u + 0u, accessUnits,
                                       static_cast<uint32_t>(range.length)))
                ++messageFailures;
        }
        if (idrWithRefs || badIdrFields || messageFailures || rejected) ++failures;
        std::printf("%s: access units=%u idrs=%u rejected=%u idr_with_references=%u idr_bad_fields=%u message_failures=%u\n",
                    argv[a], accessUnits, idrs, rejected, idrWithRefs, badIdrFields, messageFailures);
    }
    return failures ? 1 : 0;
}
