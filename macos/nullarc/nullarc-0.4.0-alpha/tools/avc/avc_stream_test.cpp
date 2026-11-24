// SPDX-License-Identifier: MIT
#include "avc_stream_parser.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

struct Nal {
    size_t start, header;
    uint8_t type;
};

static std::vector<Nal> splitNals(const uint8_t *data, size_t size) {
    std::vector<Nal> nals;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] || data[i+1] || data[i+2] != 1) continue;
        size_t start = i && !data[i-1] ? i-1 : i;
        nals.push_back({start, i+3, static_cast<uint8_t>(data[i+3] & 31u)});
        i += 2;
    }
    return nals;
}

static void checkRanges(const uint8_t *au, size_t size, const std::vector<uint8_t> &sets,
                        const phx_vcn::AvcStreamParser &before, bool idr) {
    const uint8_t type = idr ? 5 : 1;
    const auto nals = splitNals(au, size);
    std::vector<size_t> slices;
    for (const auto &nal : nals)
        if (nal.type == type) slices.push_back(nal.start);
    assert(slices.size() == 4);
    std::vector<uint8_t> request(sets);
    const size_t start = request.size();
    request.insert(request.end(), au+slices.front(), au+size);
    const size_t length = request.size()-start;
    const std::vector<uint8_t> filler{0,0,1,12,0xff,0x80};
    request.insert(request.end(), filler.begin(), filler.end());
    phx_vcn::AvcStreamSequence sequence{};
    phx_vcn::AvcStreamPicture picture{};
    phx_vcn::AvcVclRange range{10, 20};
    auto parser = before;
    uint32_t count = 17;
    // A gap between slices cannot be submitted as one VCL-only byte span.
    const size_t gap = start + slices[1] - slices[0];
    auto broken = request;
    broken.insert(broken.begin()+static_cast<ptrdiff_t>(gap), filler.begin(), filler.end());
    assert(!parser.parse(broken.data(), broken.size(), sequence, picture, count, &range));
    assert(range.offset == 10 && range.length == 20 && count == 17);
    if (!idr) {
        // A well-formed SPS with a different level must wait for an IDR.
        auto switched = request;
        const auto headers = splitNals(sets.data(), sets.size());
        assert(headers.size() == 2 && headers[0].type == 7);
        uint8_t &level = switched[headers[0].header+3];
        level = level == 40 ? 41 : 40;
        assert(!parser.parse(switched.data(), switched.size(), sequence, picture, count, &range));
        assert(range.offset == 10 && range.length == 20 && count == 17);
        // This fixture has PPS and SPS id zero, each encoded as one '1' bit.
        // Changing the following entropy_coding_mode_flag gives a distinct PPS.
        switched = request;
        assert(headers[1].type == 8 && (switched[headers[1].header+1] & 0xc0u) == 0xc0u);
        switched[headers[1].header+1] ^= 0x20u;
        assert(!parser.parse(switched.data(), switched.size(), sequence, picture, count, &range));
        assert(range.offset == 10 && range.length == 20 && count == 17);
    }
    assert(parser.parse(request.data(), request.size(), sequence, picture, count, &range));
    assert(picture.idr == idr && count == 4 && range.offset == start && range.length == length);
    assert(std::equal(au+slices.front(), au+size, request.data()+range.offset));

    // Exercise both Annex B prefix lengths even if the encoder only emits one
    // length on slices. Internal slice prefixes and payloads stay intact.
    parser = before;
    if (request[start+2] == 1) {
        request.insert(request.begin()+static_cast<ptrdiff_t>(start), 0);
        assert(parser.parse(request.data(), request.size(), sequence, picture, count, &range));
        assert(range.offset == start && range.length == length+1);
    } else {
        request.erase(request.begin()+static_cast<ptrdiff_t>(start));
        assert(parser.parse(request.data(), request.size(), sequence, picture, count, &range));
        assert(range.offset == start && range.length == length-1);
    }
}

static void testStream(const char *path, uint32_t width, uint32_t height) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<size_t> offsets;
    for (size_t i = 0; i + 5 <= bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 0 &&
            bytes[i+3] == 1 && bytes[i+4] == 9) offsets.push_back(i);
    }
    offsets.push_back(bytes.size());
    assert(offsets.front() == 0 && offsets.size() >= 3);
    phx_vcn::AvcStreamParser parser;
    phx_vcn::AvcStreamSequence sequence{};
    phx_vcn::AvcStreamPicture picture{};
    uint32_t slices = 0;
    std::vector<uint8_t> sets;
    assert(!parser.parse(bytes.data() + offsets[1], offsets[2] - offsets[1], sequence, picture, slices));
    for (size_t i = 0; i + 1 < offsets.size(); ++i) {
        const uint8_t *au = bytes.data() + offsets[i];
        size_t size = offsets[i + 1] - offsets[i];
        if (i % 12 == 0) {
            sets.clear();
            const auto nals = splitNals(au, size);
            for (size_t n = 0; n+1 < nals.size(); ++n) {
                if (nals[n].type == 7 || nals[n].type == 8)
                    sets.insert(sets.end(), au+nals[n].start, au+nals[n+1].start);
            }
        }
        checkRanges(au, size, sets, parser, i % 12 == 0);
        assert(parser.parse(au, size, sequence, picture, slices));
        assert(sequence.visibleWidth == width && sequence.visibleHeight == height);
        assert(sequence.codedWidth == width && sequence.codedHeight == (height + 15) / 16 * 16);
        assert(sequence.maxNumRefFrames == 1 && slices == 4);
        assert(picture.idr == (i % 12 == 0));
        assert(picture.frameNum == i % 12 && picture.topPoc == static_cast<int32_t>(2 * (i % 12)));
        assert(picture.bottomPoc == picture.topPoc);
        if (!picture.idr) {
            assert(picture.referenceCount == 1);
            assert(picture.references[0].frameNum + 1 == picture.frameNum);
            assert(picture.references[0].topPoc + 2 == picture.topPoc);
            assert(picture.references[0].slot != picture.currentSlot);
        }
        const auto frame = picture.frameNum;
        assert(!parser.parse(au, 4, sequence, picture, slices));
        assert(picture.frameNum == frame && slices == 4);
    }
    std::printf("%s: %zu access units, %ux%u, IDR and P metadata match\n",
                path, offsets.size() - 1, width, height);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    testStream(argv[1], 864, 480);
    testStream(argv[2], 1920, 1080);
}
