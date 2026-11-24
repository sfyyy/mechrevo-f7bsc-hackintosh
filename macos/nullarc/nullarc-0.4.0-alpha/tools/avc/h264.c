#include "avd_h264.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void checkAccessUnit(const char *path, uint32_t width, uint32_t height, uint32_t slices) {
    uint8_t bytes[8192];
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t count = fread(bytes, 1, sizeof(bytes) - 3, file);
    assert(count > 4 && fgetc(file) == EOF && !ferror(file) && !fclose(file));

    PhoenixH264AccessUnit picture;
    assert(PhoenixH264ReadIDR(bytes, count, NULL, 0, NULL, 0, &picture));
    assert(picture.sps.width == width && picture.sps.height == height);
    assert(picture.sps.codedWidth == (width + 15) / 16 * 16);
    assert(picture.sps.codedHeight == (height + 15) / 16 * 16);
    assert(picture.pps.spsID == picture.sps.id && picture.sps.chromaFormat == 1);
    assert(picture.sliceCount == slices && picture.frameNum == 0);
    printf("%ux%u, profile=%u, level=%u, slices=%u, pic_order_cnt_type=%u\n",
           picture.sps.width, picture.sps.height, picture.sps.profile, picture.sps.level,
           picture.sliceCount, picture.sps.picOrderCountType);

    // Cut off the AU prior to a NAL header, or change the forbidden-zero bit.
    size_t prefix = bytes[2] == 1 ? 3 : 4;
    assert(!PhoenixH264ReadIDR(bytes, prefix, NULL, 0, NULL, 0, &picture));
    bytes[prefix] ^= 0x80;
    assert(!PhoenixH264ReadIDR(bytes, count, NULL, 0, NULL, 0, &picture));
    bytes[prefix] ^= 0x80;
    memcpy(bytes + count, (uint8_t[]){0, 0, 1}, 3);
    assert(!PhoenixH264ReadIDR(bytes, count + 3, NULL, 0, NULL, 0, &picture));

    // A subsequent IDR access unit must not be combined with the first.
    assert(count <= sizeof(bytes) / 2);
    memcpy(bytes + count, bytes, count);
    assert(!PhoenixH264ReadIDR(bytes, count * 2, NULL, 0, NULL, 0, &picture));
    if (width != 864) return;
    assert(picture.vclOffset == 22 && picture.vclLength == count - 22);

    // The reference fixture uses 4-byte SPS/PPS start codes and the first VCL
    // NAL begins at offset 22. Exercise the separate SPS/PPS entry points.
    const uint8_t *sps = bytes + 4, *pps = bytes + 18;
    PhoenixH264SPS seq;
    PhoenixH264PPS pic;
    assert(PhoenixH264ReadSPS(sps, 10, &seq) && seq.height == 480);
    assert(PhoenixH264ReadPPS(pps, 4, &pic) && pic.spsID == seq.id);
    for (size_t n = 0; n < 10; ++n) assert(!PhoenixH264ReadSPS(sps, n, &seq));
    for (size_t n = 0; n < 4; ++n) assert(!PhoenixH264ReadPPS(pps, n, &pic));
    assert(PhoenixH264ReadIDR(bytes + 22, count - 22, sps, 10, pps, 4, &picture));
    assert(picture.vclOffset == 0 && picture.vclLength == count - 22);
    assert(picture.sps.width == 864 && picture.sliceCount == 5 && picture.sps.picOrderCountType == 0);
    assert(!PhoenixH264ReadIDR(bytes + 22, count - 22, sps, 10, NULL, 0, &picture));
}

static void checkCroppedStream(const char *path) {
    uint8_t bytes[128 * 1024];
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t count = fread(bytes, 1, sizeof(bytes), file);
    assert(count && !ferror(file) && !fclose(file));
    size_t nextAU = 4;
    while (nextAU + 5 < count) {
        if (!memcmp(bytes + nextAU, (uint8_t[]){0, 0, 0, 1, 9}, 5)) break;
        ++nextAU;
    }
    assert(nextAU + 5 < count);
    PhoenixH264AccessUnit picture;
    assert(PhoenixH264ReadIDR(bytes, nextAU, NULL, 0, NULL, 0, &picture));
    assert(picture.sps.codedWidth == 1920 && picture.sps.codedHeight == 1088);
    assert(picture.sps.width == 1920 && picture.sps.height == 1080 && picture.sliceCount == 4);
    assert(!PhoenixH264ReadIDR(bytes, count, NULL, 0, NULL, 0, &picture));
    puts("1920x1088 coded, 1920x1080 visible, four IDR slices");
}

static void checkInterHeaders(void) {
    PhoenixH264SPS s = {.codedWidth = 32, .codedHeight = 16, .picOrderCountType = 2, .maxNumRefFrames = 4};
    PhoenixH264PPS p = {0};
    PhoenixH264AccessUnit au;
    // Short CAVLC P slices at frame_num 1, each with one MMCO and a skip run.
    // Each RBSP contains the operation-specific arguments, the terminating
    // MMCO 0, QP delta, skip run, and trailing bits.
    static const uint8_t operations[][3] = {
        {0xe2, 0x57, 0x50}, {0xe2, 0x5f, 0x50}, {0xe2, 0x49, 0xea},
        {0xe2, 0x4b, 0xd4}, {0xe2, 0x4d, 0xa8}, {0xe2, 0x4f, 0xd4}
    };
    uint8_t pair[] = {0,0,1,0x41, 0xe2,0x57,0x50, 0,0,1,0x41, 0x58,0x95,0xd4};
    assert(PhoenixH264ReadInter(pair, sizeof(pair), &s, &p, &au));
    assert(au.sliceType == PhoenixH264SliceP && au.isReference && au.sliceCount == 2);
    assert(au.adaptiveRefPicMarking && au.mmco1Count == 1 && au.mmco1DifferenceMinus1[0] == 0);
    assert(!PhoenixH264ReadP(pair, 7, &s, &p, &au));
    assert(!PhoenixH264ReadInter(pair, 5, &s, &p, &au));
    // Different difference_of_pic_nums_minus1 on the second slice is invalid.
    pair[12] = 0x94;
    pair[13] = 0xb5;
    assert(!PhoenixH264ReadInter(pair, sizeof(pair), &s, &p, &au));
    for (size_t i = 1; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        memcpy(pair + 4, operations[i], sizeof(operations[i]));
        assert(!PhoenixH264ReadInter(pair, 7, &s, &p, &au)); // MMCO 2..6
    }
    const uint8_t b[] = {0,0,1,0x01, 0xa8,0xc5,0x40};
    p.weightedBipredIDC = 2;
    assert(PhoenixH264ReadInter(b, sizeof(b), &s, &p, &au));
    assert(au.sliceType == PhoenixH264SliceB && !au.isReference && !au.adaptiveRefPicMarking && !au.mmco1Count);
    // Modify L0 on P and L1 on B. For the modified entry, the luma weight and
    // offset are (1,-1), the Cb/Cr pairs (0,-2) and (1,3).
    const uint8_t weightedP[] = {0,0,1,0x41, 0xe2,0xe4,0xe9,0xe5,0x46,0x54};
    const uint8_t weightedB[] = {0,0,1,0x01, 0xa8,0xce,0x4c,0xa7,0x95,0x1a,0xa0};
    p.weightedPred = true;
    p.weightedBipredIDC = 1;
    assert(PhoenixH264ReadInter(weightedP, sizeof(weightedP), &s, &p, &au));
    assert(PhoenixH264ReadInter(weightedB, sizeof(weightedB), &s, &p, &au));
    assert(!PhoenixH264ReadInter(weightedB, 8, &s, &p, &au));
    p.weightedPred = false;
    const uint8_t longTermList[] = {0,0,1,0x41, 0xe2,0xb9,0x15};
    const uint8_t excessiveList[] = {0,0,1,0x41, 0xe2,0xf9,0x15};
    assert(!PhoenixH264ReadInter(longTermList, sizeof(longTermList), &s, &p, &au));
    assert(!PhoenixH264ReadInter(excessiveList, sizeof(excessiveList), &s, &p, &au));
}

// An AUD-delimited stream, e.g. FFmpeg's h264_mp4toannexb,h264_metadata=aud=insert.
static void checkInterStream(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long fileSize = ftell(file);
    assert(fileSize > 0 && !fseek(file, 0, SEEK_SET));
    size_t size = (size_t)fileSize;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && !fclose(file));
    unsigned counts[3] = {0}, referenceB = 0, removals = 0;
    PhoenixH264AccessUnit au = {0};
    for (size_t start = 0, next; start < size; start = next) {
        for (next = start + 4; next + 3 < size; ++next) {
            if (!memcmp(bytes + next, (uint8_t[]){0, 0, 1, 9}, 4)) {
                if (bytes[next - 1] == 0) --next;
                break;
            }
        }
        if (next + 3 >= size) next = size;
        if (!PhoenixH264ReadIDR(bytes + start, next - start, NULL, 0, NULL, 0, &au)) {
            assert(counts[PhoenixH264SliceI]);
            assert(PhoenixH264ReadInter(bytes + start, next - start, &au.sps, &au.pps, &au));
        }
        ++counts[au.sliceType];
        if (au.sliceType == PhoenixH264SliceB && au.isReference) ++referenceB;
        removals += au.mmco1Count;
    }
    free(bytes);
    assert(counts[PhoenixH264SliceI] && counts[PhoenixH264SliceP] && referenceB &&
           referenceB < counts[PhoenixH264SliceB] && removals);
    printf("%ux%u, I=%u P=%u B=%u (reference B=%u), MMCO 1=%u\n", au.sps.width, au.sps.height,
           counts[PhoenixH264SliceI], counts[PhoenixH264SliceP], counts[PhoenixH264SliceB], referenceB, removals);
}

int main(int argc, char **argv) {
    checkInterHeaders();
    if (argc == 3 && !strcmp(argv[1], "--inter")) {
        checkInterStream(argv[2]);
        return 0;
    }
    assert(argc == 5 || argc == 6);
    checkAccessUnit(argv[1], 864, 480, 5);
    checkAccessUnit(argv[2], 160, 128, 2);
    checkAccessUnit(argv[3], 2048, 1152, 8);
    checkAccessUnit(argv[4], 2560, 1440, 8);
    if (argc == 6) checkCroppedStream(argv[5]);
    return 0;
}
