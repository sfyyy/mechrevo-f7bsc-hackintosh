#include "avc_parameters.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <os/log.h>
#include <stdio.h>
#include <string.h>

// A rejected export leaves the caller with a black frame and no explanation; the
// AppleGVA parameter blocks are the only place that says which syntax field is
// out of contract, so summarize the offending words on failure.
static void logNonzeroWords(const char *tag, const uint32_t *words, size_t from, size_t to) {
    char line[480] = {0};
    size_t used = 0, count = 0;
    for (size_t offset = from; offset < to; offset += 4) {
        uint32_t value = words[offset / 4];
        if (!value) continue;
        ++count;
        if (used < sizeof(line) - 24)
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %zX=%X", offset, value);
    }
    os_log(OS_LOG_DEFAULT, "Phoenix AVC export %{public}s nonzero=%{public}zu%{public}s", tag, count, line);
}

bool PhoenixAVDCompatibleAppleGVA(void) {
    static const uint8_t uuid[] = {0xca,0x54,0x14,0xf3,0x69,0x37,0x3c,0x05,0x9e,0xa2,0xd8,0x2b,0x35,0xaf,0xd4,0xf0};
    static atomic_bool verified;
    if (atomic_load_explicit(&verified, memory_order_relaxed)) return true;
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!path || strcmp(path, "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA")) continue;
        const struct mach_header_64 *header = (const void *)_dyld_get_image_header(i);
        if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_X86_64) return false;
        const uint8_t *cmd = (const void *)(header + 1);
        size_t remaining = header->sizeofcmds;
        for (uint32_t j = 0; j < header->ncmds && remaining >= sizeof(struct load_command); ++j) {
            const struct load_command *lc = (const void *)cmd;
            if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > remaining) return false;
            if (lc->cmd == LC_UUID && lc->cmdsize == sizeof(struct uuid_command)) {
                if (memcmp(((const struct uuid_command *)lc)->uuid, uuid, sizeof(uuid))) return false;
                atomic_store_explicit(&verified, true, memory_order_relaxed);
                return true;
            }
            remaining -= lc->cmdsize;
            cmd += lc->cmdsize;
        }
    }
    return false;
}

typedef struct {
    uint8_t bytes[PhoenixAVCParameterCapacity / 2];
    size_t bits;
} RBSP;

static void putBits(RBSP *b, uint32_t value, unsigned n) {
    for (unsigned i = n; i > 0; --i, ++b->bits)
        if (b->bits < sizeof(b->bytes) * 8)
            b->bytes[b->bits / 8] |= ((value >> (i - 1)) & 1u) << (7 - b->bits % 8);
}

static void putUE(RBSP *b, uint32_t value) {
    uint32_t code = value + 1;
    unsigned width = 32 - (unsigned)__builtin_clz(code);
    putBits(b, 0, width - 1);
    putBits(b, code, width);
}

static void putSE(RBSP *b, int32_t value) {
    putUE(b, (uint32_t)(value > 0 ? 2 * value - 1 : -2 * value));
}

static bool appendNAL(RBSP *b, uint8_t header, uint8_t *out, size_t *used) {
    putBits(b, 1, 1); // rbsp_trailing_bits, with the rest of the final byte already zero
    size_t bytes = (b->bits + 7) / 8;
    if (bytes > sizeof(b->bytes) || *used > PhoenixAVCParameterCapacity - 5) return false;
    const uint8_t prefix[] = {0,0,0,1,header};
    memcpy(out + *used, prefix, sizeof(prefix));
    *used += sizeof(prefix);
    unsigned zeros = 0;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte = b->bytes[i];
        if (zeros == 2 && byte <= 3) {
            if (*used == PhoenixAVCParameterCapacity) return false;
            out[(*used)++] = 3;
            zeros = 0;
        }
        if (*used == PhoenixAVCParameterCapacity) return false;
        out[(*used)++] = byte;
        zeros = byte ? 0 : zeros + 1;
    }
    return true;
}

// Only fields consumed here may be nonzero. The remainder includes the custom
// scaling lists, interlaced syntax, FMO maps and HRD tables of this AppleGVA ABI.
static bool zeroWords(const uint32_t *words, size_t from, size_t to) {
    for (size_t i = from / 4; i < to / 4; ++i)
        if (words[i]) return false;
    return true;
}

#define F(offset) a[(offset) / 4]
static bool encodeVUI(const uint32_t *a, RBSP *bits) {
    // Layout relative to the parsed VUI at SPS+0x854, from AppleGVA +0x85ba.
    if (F(0) != 1 || F(4) > 1 || (F(8) > 16 && F(8) != 255) || (!F(4) && F(8)) ||
        F(0x14) > 1 || F(0x18) > 1 || (!F(0x14) && F(0x18)) ||
        F(0x1c) > 1 || F(0x20) > 5 || F(0x24) > 1 || F(0x28) > 1 ||
        F(0x2c) > 255 || F(0x30) > 255 || F(0x34) > 255 ||
        (!F(0x1c) && !zeroWords(a, 0x20, 0x38)) || (!F(0x28) && !zeroWords(a, 0x2c, 0x38)) ||
        !zeroWords(a, 0x38, 0x44) || F(0x44) > 1 || F(0x50) > 1 ||
        F(0x3a4) > 1 || F(0x3a8) > 1) return false;
    // 0x54..0x3A0 holds the parsed NAL and VCL HRD tables. The synthesized SPS
    // carries no HRD, so they are dropped rather than validated: encoders that
    // run CBR for a live stream (NAL HRD present, SEI buffering period and
    // picture timing in every IDR) fill this range, and demanding it stay zero
    // rejected every such stream. Macroblock-level decoding does not read HRD.
    putBits(bits, F(4), 1);
    if (F(4)) putBits(bits, F(8), 8);
    if (F(8) == 255) {
        if (!F(0xc) || F(0xc) > 65535 || !F(0x10) || F(0x10) > 65535) return false;
        putBits(bits, F(0xc), 16);
        putBits(bits, F(0x10), 16);
    } else if (F(0xc) || F(0x10)) return false;
    putBits(bits, F(0x14), 1);
    if (F(0x14)) putBits(bits, F(0x18), 1);
    putBits(bits, F(0x1c), 1);
    if (F(0x1c)) {
        putBits(bits, F(0x20), 3);
        putBits(bits, F(0x24), 1);
        putBits(bits, F(0x28), 1);
        if (F(0x28)) {
            putBits(bits, F(0x2c), 8);
            putBits(bits, F(0x30), 8);
            putBits(bits, F(0x34), 8);
        }
    }
    putBits(bits, 0, 1); // No chroma sample location.
    putBits(bits, F(0x44), 1);
    if (F(0x44)) {
        if (!F(0x48) || !F(0x4c)) return false;
        putBits(bits, F(0x48), 32);
        putBits(bits, F(0x4c), 32);
        putBits(bits, F(0x50), 1);
    } else if (!zeroWords(a, 0x48, 0x54)) return false;
    putBits(bits, 0, 3); // No NAL HRD, VCL HRD or pic_struct_present_flag.
    putBits(bits, F(0x3a4), 1);
    if (!F(0x3a4)) return zeroWords(a, 0x3a8, 0x3c4);
    putBits(bits, F(0x3a8), 1);
    // Apple stores the vertical MV bound before the horizontal bound.
    const size_t fields[] = {0x3ac, 0x3b0, 0x3b8, 0x3b4, 0x3bc, 0x3c0};
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        if (F(fields[i]) > 16) return false;
        putUE(bits, F(fields[i]));
    }
    return F(0x3bc) <= F(0x3c0);
}

static bool encodeSPS(const uint32_t *a, uint32_t spsID, RBSP *bits) {
    // AppleGVA +0x8d04, progressive 8-bit 4:2:0, POC type 0 or 2.
    if (F(0) != 1 || (F(4) != 66 && F(4) != 77 && F(4) != 100) ||
        F(8) > 1 || F(0xc) > 1 || F(0x10) > 1 || F(0x14) > 1 || !zeroWords(a, 0x18, 0x24) ||
        !F(0x24) || F(0x24) > 62 || F(0x28) || spsID > 31 || F(0x2c) != 1 ||
        !zeroWords(a, 0x30, 0x404) || F(0x404) > 12 || (F(0x408) != 0 && F(0x408) != 2) ||
        F(0x40c) > 12 || (F(0x408) && F(0x40c)) || !zeroWords(a, 0x410, 0x820) ||
        F(0x820) > 16 || F(0x824) > 1 || F(0x828) >= 4096 || F(0x82c) >= 4096 ||
        F(0x830) != 1 || F(0x834) || F(0x838) > 1 || F(0x83c) > 1 || F(0x850) > 1)
        return false;
    putBits(bits, F(4), 8);
    putBits(bits, F(8) << 7 | F(0xc) << 6 | F(0x10) << 5 | F(0x14) << 4, 8);
    putBits(bits, F(0x24), 8);
    // This AppleGVA stores the selected SPS ID in the paired PPS, not the SPS block.
    putUE(bits, spsID);
    if (F(4) == 100) {
        putUE(bits, 1); // chroma_format_idc
        putUE(bits, 0);
        putUE(bits, 0);
        putBits(bits, 0, 2); // Transform bypass and scaling matrix absent.
    }
    putUE(bits, F(0x404));
    putUE(bits, F(0x408));
    if (!F(0x408)) putUE(bits, F(0x40c));
    putUE(bits, F(0x820));
    putBits(bits, F(0x824), 1);
    putUE(bits, F(0x828));
    putUE(bits, F(0x82c));
    putBits(bits, 1, 1);
    putBits(bits, F(0x838), 1);
    putBits(bits, F(0x83c), 1);
    if (F(0x83c)) {
        uint64_t x = (uint64_t)F(0x840) + F(0x844), y = (uint64_t)F(0x848) + F(0x84c);
        if (x >= (F(0x828) + 1) * 8 || y >= (F(0x82c) + 1) * 8) return false;
        for (unsigned i = 0x840; i <= 0x84c; i += 4) putUE(bits, F(i));
    } else if (!zeroWords(a, 0x840, 0x850)) return false;
    putBits(bits, F(0x850), 1);
    return F(0x850) ? encodeVUI(a + 0x854 / 4, bits) : zeroWords(a, 0x854, 0xc18);
}

static bool encodePPS(const uint32_t *a, uint32_t profile, RBSP *bits) {
    if (F(0) != 1 || F(4) > 255 || F(8) > 31 || F(0xc) > 1 || F(0x10) > 1 ||
        !zeroWords(a, 0x14, 0x90) || F(0x90) > 31 || F(0x94) > 31 ||
        F(0x98) > 1 || F(0x9c) > 2 || F(0xac) > 1 || F(0xb0) > 1 || F(0xb4) > 1 ||
        F(0xb8) > 1 || !zeroWords(a, 0xbc, 0x480) || F(0x484)) return false;
    int32_t qp = (int32_t)F(0xa0), qs = (int32_t)F(0xa4), chroma = (int32_t)F(0xa8);
    int32_t secondChroma = (int32_t)F(0x480);
    if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || chroma < -12 || chroma > 12 ||
        secondChroma < -12 || secondChroma > 12) return false;
    if ((profile != 100 && (F(0xb8) || secondChroma != chroma)) ||
        (profile == 66 && (F(0xc) || F(0x98) || F(0x9c)))) return false;
    putUE(bits, F(4));
    putUE(bits, F(8));
    putBits(bits, F(0xc), 1);
    putBits(bits, F(0x10), 1);
    putUE(bits, 0); // num_slice_groups_minus1
    putUE(bits, F(0x90));
    putUE(bits, F(0x94));
    putBits(bits, F(0x98), 1);
    putBits(bits, F(0x9c), 2);
    putSE(bits, qp);
    putSE(bits, qs);
    putSE(bits, chroma);
    putBits(bits, F(0xac), 1);
    putBits(bits, F(0xb0), 1);
    putBits(bits, F(0xb4), 1);
    if (F(0xb8) || secondChroma != chroma) {
        putBits(bits, F(0xb8), 1);
        putBits(bits, 0, 1);
        putSE(bits, secondChroma);
    }
    return true;
}
#undef F

bool PhoenixAVDExportAVCParameters(const uint32_t *sps, const uint32_t *pps,
                                   uint8_t *annexB, size_t capacity, size_t *written) {
    if (!written) return false;
    *written = 0;
    if (!sps || !pps || !annexB || !PhoenixAVDCompatibleAppleGVA()) return false;
    uint8_t result[PhoenixAVCParameterCapacity];
    RBSP seq = {0}, pic = {0};
    size_t used = 0;
    if (!encodeSPS(sps, pps[0x8 / 4], &seq) || !encodePPS(pps, sps[0x4 / 4], &pic) ||
        !appendNAL(&seq, 0x67, result, &used) || !appendNAL(&pic, 0x68, result, &used) || used > capacity) {
        os_log(OS_LOG_DEFAULT,
               "Phoenix AVC export failed sps=spsID=%{public}u entry=%{public}u profile=%{public}u level=%{public}u"
               " poc=%{public}u refs=%{public}u wMB=%{public}u hMB=%{public}u crop=%{public}u,%{public}u,%{public}u,%{public}u"
               " vui=%{public}u pps=id=%{public}u sid=%{public}u",
               pps[0x8 / 4], sps[0], sps[0x4 / 4], sps[0x24 / 4], sps[0x408 / 4], sps[0x820 / 4],
               sps[0x828 / 4], sps[0x82c / 4], sps[0x840 / 4], sps[0x844 / 4], sps[0x848 / 4], sps[0x84c / 4],
               sps[0x850 / 4], pps[0x4 / 4], pps[0x8 / 4]);
        logNonzeroWords("sps-header", sps, 0x8, 0x30);
        logNonzeroWords("sps-scaling", sps, 0x30, 0x404);
        logNonzeroWords("sps-sequence", sps, 0x404, 0x854);
        logNonzeroWords("sps-vui-head", sps + 0x854 / 4, 0, 0x60);
        logNonzeroWords("sps-vui-rest", sps + 0x854 / 4, 0x54, 0x3c8);
        logNonzeroWords("pps-head", pps, 0, 0x20);
        logNonzeroWords("pps-lists", pps, 0x14, 0x90);
        logNonzeroWords("pps-rest", pps, 0xbc, 0x488);
        return false;
    }
    memcpy(annexB, result, used);
    *written = used;
    return true;
}
