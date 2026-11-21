/*
 * vcnctl — drive the hard-coded VCN 4.0.2 bring-up in PhxBringup.kext (debug selector 120).
 *
 *   sudo vcnctl status                 # what the block and the ring currently say
 *   sudo vcnctl init <vcn_4_0_0.bin>   # stage the firmware, PSP load, SMU power-up, VCPU boot, ring enable
 *   sudo vcnctl ring [n]               # submit n VCN_ENC_CMD_END packets and watch UVD_RB_RPTR move
 *   sudo vcnctl down                   # disable the ring and send SMU PowerDownVcn
 *
 * Needs phxdebug=1 (already in this machine's boot-args) and root. Selector numbers and result codes are in
 * src/phxbringup/PhxNativeABI.h (PHXN_SEL_DBG_VCN).
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UC_TYPE 0x4E34384Eu
#define NATIVE_ABI_VERSION 1u
#define SEL_HELLO 0u
#define SEL_VCN 120u
#define SEL_STAGE 104u

#define PHX_VCN_OK 0u
#define PHX_VCN_ALREADY 1u
#define PHX_VCN_NOT_READY 2u
#define PHX_VCN_STOPPED 16u

static io_connect_t gConn = 0;
static uint64_t gBuild;

static int openKext(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!svc) {
        fprintf(stderr, "PhxBringup not found: is the kext loaded?\n");
        return 1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), UC_TYPE, &gConn);
    IOObjectRelease(svc);
    if (kr) {
        fprintf(stderr, "IOServiceOpen failed: 0x%08x\n", kr);
        return 1;
    }
    const uint64_t in[] = {NATIVE_ABI_VERSION, 0};
    uint64_t out[4];
    uint32_t count = 4;
    kr = IOConnectCallScalarMethod(gConn, SEL_HELLO, in, 2, out, &count);
    if (kr || count != 4 || out[0] != NATIVE_ABI_VERSION) {
        if (kr)
            fprintf(stderr, "PhxBringup HELLO failed: 0x%08x (%s)\n", kr, mach_error_string(kr));
        else
            fputs("PhxBringup returned an incompatible HELLO reply\n", stderr);
        IOServiceClose(gConn);
        gConn = IO_OBJECT_NULL;
        return 1;
    }
    gBuild = out[1];
    return 0;
}

static uint64_t gSramPadArg;   // extra scalar for --sram-pad, carried on every op

static int vcnOp(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t out[8]) {
    uint64_t in[6] = {op, a, b, c, d, gSramPadArg};
    uint32_t n = 8;
    // Builds 37/38 shipped the original five-scalar diagnostic ABI.
    const uint32_t inputs = gBuild <= 38 ? 5 : 6;
    const kern_return_t kr = IOConnectCallScalarMethod(gConn, SEL_VCN, in, inputs, out, &n);
    if (kr) {
        fprintf(stderr, "selector %u op %llu failed: kr 0x%08x (%s)\n", SEL_VCN, (unsigned long long)op,
                kr, mach_error_string(kr));
        return 1;
    }
    if (n != 8) {
        fprintf(stderr, "selector %u returned %u scalars, expected 8\n", SEL_VCN, n);
        return 1;
    }
    return 0;
}

static const char *codeName(uint64_t c) {
    switch (c) {
    case 0: return "ok";
    case 1: return "already up";
    case 2: return "not ready (VCN down, sleeping, or PSP/TMR not ready)";
    case 3: return "bad argument";
    case 4: return "bad firmware header";
    case 5: return "PSP LOAD_IP_FW failed";
    case 6: return "PSP returned no usable TMR address";
    case 7: return "SMU did not acknowledge PowerUpVcn";
    case 8: return "VCN planes did not power on";
    case 9: return "VCN registers read all ones";
    case 10: return "VCPU did not report firmware ready";
    case 11: return "power-down failed";
    case 12: return "ring not enabled";
    case 13: return "UVD_RB_RPTR did not reach the write pointer";
    case 14: return "firmware did not write the fence";
    case 15: return "write pointer did not take the value";
    case 16: return "stopped at the requested stage (VCN powered down again)";
    case 17: return "VCN0 NBIF doorbell range or BAR2 unavailable";
    case 18: return "frame scratch unavailable";
    case 19: return "frame payload failed validation";
    default: return "unknown";
    }
}

static void printOut(const char *tag, const uint64_t o[8]) {
    printf("%s: code %llu (%s)\n", tag, (unsigned long long)o[0], codeName(o[0]));
    printf("  detail          0x%llx\n", (unsigned long long)o[1]);
    printf("  ucode           offset 0x%llx size %llu\n", (unsigned long long)o[2], (unsigned long long)o[3]);
    printf("  firmware TMR MC 0x%llx\n", (unsigned long long)o[4]);
    printf("  UVD_STATUS      0x%08x\n", (uint32_t)o[5]);
    printf("  UVD_PGFSM_STATUS 0x%08x\n", (uint32_t)o[6]);
    printf("  ring            rptr %u wptr %u\n", (uint32_t)o[7], (uint32_t)(o[7] >> 32));
}

static int cmdStatus(void) {
    uint64_t o[8];
    if (vcnOp(1, 0, 0, 0, 0, o))
        return 1;
    printf("VCN: %s\n", o[0] ? "up" : "down");
    printf("  fw_shared flags 0x%08x  sq 0x%08x (enabled %u, mode %u, status %u)\n", (uint32_t)o[1], (uint32_t)o[2],
           (unsigned)(o[2] & 1), (unsigned)((o[2] >> 8) & 0xFF), (unsigned)((o[2] >> 16) & 0xFF));
    printf("  fw TMR MC       0x%llx\n", (unsigned long long)o[4]);
    printf("  UVD_STATUS      0x%08x\n", (uint32_t)o[5]);
    printf("  UVD_PGFSM_STATUS 0x%08x\n", (uint32_t)o[6]);
    printf("  ring            rptr %u wptr %u\n", (uint32_t)o[7], (uint32_t)(o[7] >> 32));
    return 0;
}

// Stage a file into fw_buf and issue GFX_CMD_ID_LOAD_TOC (op 7). Linux sends this once at PSP bring-up.
static int cmdToc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { perror("seek"); fclose(f); return 1; }
    const long len = ftell(f);
    if (len <= 0 || len > (1 << 20)) {
        fprintf(stderr, "%s: implausible size %ld\n", path, len);
        fclose(f);
        return 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { perror("seek to start"); fclose(f); return 1; }
    // Linux sends the TOC payload only: ucode_array_offset_bytes .. +ucode_size_bytes (parse_psp_v1_bin_descriptor),
    // not the whole file. common_firmware_header: size, header_size, ver, ipver, ucode_version, ucode_size, ucode_off.
    uint32_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fprintf(stderr, "%s: short header\n", path); fclose(f); return 1; }
    const uint32_t ucodeOff = hdr[6], ucodeSize = hdr[5];
    if (ucodeOff < sizeof(hdr) || !ucodeSize || (uint64_t)ucodeOff + ucodeSize > (uint32_t)len) {
        fprintf(stderr, "%s: payload 0x%x+0x%x exceeds file 0x%lx\n", path, ucodeOff, ucodeSize, len);
        fclose(f);
        return 1;
    }
    if (fseek(f, (long)ucodeOff, SEEK_SET) != 0) { perror("seek to payload"); fclose(f); return 1; }
    printf("%s: TOC payload at 0x%x, %u bytes (file %ld)\n", path, ucodeOff, ucodeSize, len);
    const uint32_t len32 = ucodeSize;
    uint8_t chunk[4096];
    uint32_t staged = 0;
    while (staged < len32) {
        const size_t want = len32 - staged > sizeof(chunk) ? sizeof(chunk) : len32 - staged;
        if (fread(chunk, 1, want, f) != want) { fprintf(stderr, "%s: short read\n", path); fclose(f); return 1; }
        uint64_t in[1] = {staged};
        const kern_return_t kr = IOConnectCallMethod(gConn, SEL_STAGE, in, 1, chunk, want, NULL, NULL, NULL, NULL);
        if (kr) { fprintf(stderr, "stage at 0x%x failed: kr 0x%08x\n", staged, kr); fclose(f); return 1; }
        staged += (uint32_t)want;
    }
    fclose(f);
    printf("staged %s: %u bytes into fw_buf\n", path, staged);
    uint64_t o[8] = {0};
    if (vcnOp(7, 0, staged, 0, 0, o))
        return 1;
    printf("LOAD_TOC -> status 0x%llx  detail 0x%llx (0 = the PSP accepted the TOC)\n",
           (unsigned long long)o[0], (unsigned long long)o[1]);
    return o[0] ? 1 : 0;
}

static int cmdInitRun(uint32_t bufOff, uint32_t len, uint64_t tmrMc);
static uint32_t gCrumbMode;
static uint32_t gDpg;
static uint32_t gDpgSram;    // init --dpg-sram: stage the DPG sequence and let the PSP apply it
static uint32_t gSramFull;   // init --sram-full: pad the DPG block to a full 4 KB page
static uint32_t gSramPad;    // init --sram-pad N: pad the block to N bytes (must be a multiple of 8)
static uint32_t gFwType;     // init --fw-type N: LOAD_IP_FW firmware type override
static uint32_t gDpgPg;      // init --dpg-pg: --dpg plus POWER_STATUS dynamic gating        // init --dpg: start through the DPG indirect port   // init --fast-crumbs (1) / --no-crumbs (2)
static uint32_t gStage;   // init --stage N: stop after stage N (1..7), 0 = full

// The kernel validates the staged header again; doing it here turns a wrong file into a clear message instead of
// a selector result. Mirrors phx_vcn::fwHeaderOk.
static int checkFwHeader(FILE *f, long len, uint32_t *ucodeOff, uint32_t *ucodeSize) {
    uint32_t h[8];
    if (len < (long)sizeof(h)) {
        fputs("file is shorter than a 32-byte firmware header\n", stderr);
        return 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        perror("seek to firmware header");
        return 1;
    }
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
        if (ferror(f))
            perror("read firmware header");
        else
            fputs("unexpected EOF while reading firmware header\n", stderr);
        return 1;
    }
    const uint32_t sizeBytes = h[0], headerBytes = h[1];
    const uint16_t hdrMajor = (uint16_t)h[2], hdrMinor = (uint16_t)(h[2] >> 16);
    const uint16_t ipMajor = (uint16_t)h[3], ipMinor = (uint16_t)(h[3] >> 16);
    const uint32_t usize = h[5], off = h[6];
    if (headerBytes != 32 || hdrMajor != 1 || hdrMinor != 0 || ipMajor != 4 || ipMinor != 0) {
        fprintf(stderr, "not a VCN 4.0 image: header %u.%u size %u, IP %u.%u\n", hdrMajor, hdrMinor, headerBytes,
                ipMajor, ipMinor);
        return 1;
    }
    if (off < headerBytes || (off & 3u) || usize == 0 || (uint64_t)off + usize > (uint64_t)len || sizeBytes != len) {
        fprintf(stderr, "bad layout: size_bytes %u, ucode off %u size %u, file %ld\n", sizeBytes, off, usize, len);
        return 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        perror("seek to firmware start for staging");
        return 1;
    }
    *ucodeOff = off;
    *ucodeSize = usize;
    return 0;
}

static int cmdInit(const char *path, uint32_t bufOff, uint64_t tmrMc, int checkOnly) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("seek");
        fclose(f);
        return 1;
    }
    const long len = ftell(f);
    if (len <= 0 || len > (16 << 20)) {
        fprintf(stderr, "%s: implausible size %ld\n", path, len);
        fclose(f);
        return 1;
    }
    uint32_t ucodeOff = 0, ucodeSize = 0;
    if (checkFwHeader(f, len, &ucodeOff, &ucodeSize)) {
        fclose(f);
        return 1;
    }
    printf("%s: VCN 4.0 firmware, ucode at 0x%x, %u bytes\n", path, ucodeOff, ucodeSize);
    if (checkOnly) {
        fclose(f);
        return 0;
    }
    uint8_t chunk[4096];
    uint32_t staged = 0;
    while (staged < (uint32_t)len) {
        const size_t want = (uint32_t)len - staged > sizeof(chunk) ? sizeof(chunk) : (uint32_t)len - staged;
        const size_t got = fread(chunk, 1, want, f);
        if (got != want) {
            fprintf(stderr, "%s: short read at 0x%x\n", path, staged);
            fclose(f);
            return 1;
        }
        uint64_t in[1] = {bufOff + staged};
        const kern_return_t kr = IOConnectCallMethod(gConn, SEL_STAGE, in, 1, chunk, got, NULL, NULL, NULL, NULL);
        if (kr) {
            fprintf(stderr, "stage at 0x%x failed: kr 0x%08x (is the PSP ring up? see phxboot)\n", bufOff + staged, kr);
            fclose(f);
            return 1;
        }
        staged += (uint32_t)got;
    }
    fclose(f);
    printf("staged %s: %u bytes at fw_buf+0x%x\n", path, staged, bufOff);
    return cmdInitRun(bufOff, staged, tmrMc);
}

static int cmdInitRun(uint32_t bufOff, uint32_t len, uint64_t tmrMc) {
    uint64_t o[8];
    if (gStage)
        printf("init: stopping after stage %u\n", gStage);
    fflush(stdout);   // a hard reset must not swallow what was printed before it
    if (vcnOp(2 | (gStage << 8) | ((uint64_t)gCrumbMode << 16) | ((uint64_t)gDpg << 18) | ((uint64_t)gDpgPg << 19) | ((uint64_t)gDpgSram << 20) | ((uint64_t)gSramFull << 21) | ((uint64_t)gFwType << 24), bufOff, len, tmrMc & 0xFFFFFFFFull, tmrMc >> 32, o))
        return 1;
    printOut("init", o);
    return o[0] == PHX_VCN_OK || o[0] == PHX_VCN_ALREADY || (gStage && o[0] == PHX_VCN_STOPPED) ? 0 : 1;
}

static int cmdRing(int count) {
    for (int i = 0; i < count; i++) {
        uint64_t o[8];
        if (vcnOp(3, 0, 0, 0, 0, o))
            return 1;
        printf("ring %d: code %llu (%s) rptr %llu -> %llu wptr %llu queue_status %llu UVD_STATUS 0x%08x\n", i + 1,
               (unsigned long long)o[0], codeName(o[0]), (unsigned long long)o[1], (unsigned long long)o[2],
               (unsigned long long)o[3], (unsigned long long)o[4], (uint32_t)o[5]);
        if (o[0] != PHX_VCN_OK)
            return 1;
    }
    return 0;
}

static int cmdFence(int count) {
    for (int i = 0; i < count; i++) {
        uint64_t o[8];
        if (vcnOp(5, 0, 0, 0, 0, o))
            return 1;
        printf("fence %d: code %llu (%s) seq %llu seen 0x%08x rptr %llu -> %llu UVD_STATUS 0x%08x\n", i + 1,
               (unsigned long long)o[0], codeName(o[0]), (unsigned long long)o[1],
               (uint32_t)o[2], (unsigned long long)o[4], (unsigned long long)o[5], (uint32_t)o[6]);
        if (o[0] != PHX_VCN_OK || !o[1] || o[1] != o[2])
            return 1;
    }
    return 0;
}

static int cmdCodecSession(int encode) {
    if (gBuild < 60) {
        fprintf(stderr, "session tests need build 60 (loaded %llu)\n", (unsigned long long)gBuild);
        return 1;
    }
    uint64_t o[8];
    if (vcnOp(encode ? 9 : 8, 0, 0, 0, 0, o))
        return 1;
    printf("%s session: %s, phase %llu, fence %llu observed %llu, rptr %llu -> %llu\n",
           encode ? "encode" : "decode", codeName(o[0]), (unsigned long long)o[1],
           (unsigned long long)o[2], (unsigned long long)o[3],
           (unsigned long long)o[4], (unsigned long long)o[5]);
    return o[0] != PHX_VCN_OK || o[1] != 2 || !o[2] || o[2] != o[3];
}

// Independently inspect the BO returned by the kernel before publishing the file.
static bool h264Idr(const uint8_t *data, size_t size) {
    unsigned zeroes = 0;
    bool sps = false, pps = false, idr = false, started = false;
    for (size_t i = 0; i < size; ++i) {
        if (!data[i]) {
            ++zeroes;
            continue;
        }
        if (zeroes >= 2 && data[i] == 1) {
            if (++i + 1 >= size || (data[i] & 0x80))
                return false;
            unsigned type = data[i] & 31u;
            if (!type || type > 23 || (type == 8 && !sps) || (type == 5 && !pps))
                return false;
            sps |= type == 7;
            pps |= type == 8;
            idr |= type == 5;
            started = true;
        } else if (!started) {
            return false;
        }
        zeroes = 0;
    }
    return idr;
}

static int saveFrame(const char *path, const uint8_t *bytes, size_t length) {
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        fputs("frame output path is too long\n", stderr);
        return 1;
    }
    int fd = mkstemp(temp);
    if (fd == -1) { perror(path); return 1; }
    bool ok = true;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { perror("write frame"); ok = false; break; }
        length -= (size_t)written;
        bytes += written;
    }
    if (close(fd) < 0) { perror("close frame"); ok = false; }
    if (ok && rename(temp, path) == 0) return 0;
    if (ok) perror(path);
    unlink(temp);
    return 1;
}

static int cmdCodecFrame(bool encode, const char *path) {
    if (gBuild < 62) {
        fprintf(stderr, "frame tests require build 62 or newer (loaded %llu)\n", gBuild);
        return 1;
    }
    const size_t capacity = encode ? 28672u : 737280u;
    const uint64_t request[] = {capacity, 4096, 4, 0}; // bo_size, alignment, visible VRAM domain, flags
    uint64_t bo[4] = {0};
    uint32_t count = 4;
    kern_return_t kr = IOConnectCallMethod(gConn, 3, NULL, 0, request, sizeof(request), bo, &count, NULL, NULL);
    if (kr || count != 4 || !bo[0] || bo[0] > UINT32_MAX || bo[1] < capacity) {
        fprintf(stderr, "create frame BO failed (0x%08x)\n", kr);
        return 1;
    }
    mach_vm_address_t address = 0;
    mach_vm_size_t mapped = 0;
    int rc = 1;
    kr = IOConnectMapMemory64(gConn, (uint32_t)bo[0], mach_task_self(), &address, &mapped, kIOMapAnywhere);
    if (kr || mapped < capacity) {
        fprintf(stderr, "map frame BO failed (0x%08x, %llu bytes)\n", kr, mapped);
    } else {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;
        uint64_t o[8];
        if (!vcnOp(encode ? 11 : 10, bo[0], 0, 0, 0, o)) {
            printf("%s frame: %s, phase %llu, fence %llu observed %llu, rptr %llu -> %llu, bytes %llu, %s 0x%llx\n",
                   encode ? "encode" : "decode", codeName(o[0]), o[1], o[2], o[3], o[4], o[5], o[6],
                   encode ? "feedback end" : "NV12 sum", o[7]);
            bool valid = !o[0] && o[1] == 3 && o[2] && o[2] == o[3] && o[6] && o[6] <= capacity;
            if (valid && encode) {
                valid = o[7] == o[6] && h264Idr(bytes, (size_t)o[6]);
            } else if (valid) {
                uint32_t sum = 0;
                for (size_t i = 0; i < capacity; ++i) sum += bytes[i];
                valid = o[6] == capacity && sum == 0x020345d8u && o[7] == sum;
            }
            if (valid) rc = path ? saveFrame(path, bytes, (size_t)o[6]) : 0;
            else fputs("frame result or mapped payload failed validation\n", stderr);
        }
    }
    if (!kr && IOConnectUnmapMemory64(gConn, (uint32_t)bo[0], mach_task_self(), address)) rc = 1;
    if (IOConnectCallScalarMethod(gConn, 4, bo, 1, NULL, NULL)) rc = 1;
    return rc;
}

static int cmdDown(void) {
    uint64_t o[8];
    if (vcnOp(4, 0, 0, 0, 0, o))
        return 1;
    if (o[0] == PHX_VCN_NOT_READY) {
        puts("VCN already down or sleeping");
        return 0;
    }
    printf("down: code %llu (%s), SMU response 0x%08x\n", (unsigned long long)o[0], codeName(o[0]), (uint32_t)o[1]);
    return o[0] != PHX_VCN_OK;
}

// Breadcrumbs the kext writes into VRAM before every VCN register access / SMU message (PhxBringup::vcnCrumb).
#define CRUMB_OFF 0xC900000u
#define CRUMB_N 256u
static const char *regName(uint32_t r) {
    switch (r) {
    case 0x7e00: return "PGFSM_CONFIG";   case 0x7e01: return "PGFSM_STATUS";  case 0x7e02: return "POWER_STATUS";
    case 0x7f43: return "UVD_STATUS";     case 0x7f96: return "VCPU_CNTL";     case 0x7ee1: return "MASTINT_EN";
    case 0x80ae: return "LMI_CTRL2";      case 0x7f47: return "SOFT_RESET";    case 0x80b0: return "LMI_CTRL";
    case 0x7fd4: return "MPC_CNTL";       case 0x7fd6: return "MPC_SET_MUXA0"; case 0x7fd8: return "MPC_SET_MUXB0";
    case 0x7fda: return "MPC_SET_MUX";    case 0x7e42: return "GFX10_ADDR_CONFIG"; case 0x7f06: return "RB_ARB_CTRL";
    case 0x7e72: return "RB1_DB_CTRL";    case 0x7e85: return "RB_ENABLE";     case 0x7eac: return "RB_RPTR";
    case 0x7ead: return "RB_WPTR";        case 0x7eea: return "RB_BASE_LO";    case 0x7eeb: return "RB_BASE_HI";
    case 0x7eec: return "RB_SIZE";
    case 0x80b1: return "LMI_STATUS";     case 0x7ec1: return "CGC_GATE";      case 0x7ec2: return "CGC_CTRL";
    case 0x7ec4: return "SUVD_CGC_GATE";  case 0x7f81: return "CACHE_SIZE0";   case 0x8044: return "CACHE_BAR_LO";
    case 0x8045: return "CACHE_BAR_HI";   case 0x7ed0: return "GPCOM_DATA0";   case 0x7e15: return "SCRATCH1";
    default: return "";
    }
}

static int vramRead(uint32_t off, uint32_t count, uint32_t *buf) {
    uint64_t in[2] = {off, count};
    size_t sz = count * 4;
    const kern_return_t kr = IOConnectCallMethod(gConn, 102, in, 2, NULL, 0, NULL, NULL, buf, &sz);
    if (kr) {
        fprintf(stderr, "VRAM read at 0x%x failed: 0x%x\n", off, kr);
        return 1;
    }
    return 0;
}

#define SWEEP_OFF 0xC940000u
#define SWEEP_N 0x300u
static int cmdSweep(void) {
    static uint32_t d[4 + SWEEP_N];
    const uint32_t total = 4 + SWEEP_N;
    for (uint32_t i = 0; i < total; i += 256)
        if (vramRead(SWEEP_OFF + i * 4, total - i > 256 ? 256 : total - i, d + i))
            return 1;
    if (d[0] != 0x53574356u) {
        printf("no sweep in VRAM (magic 0x%08x): the VCPU boot did not fail yet, or VRAM was cleared\n", d[0]);
        return 1;
    }
    const uint32_t first = d[1], n = d[2];
    uint32_t ones = 0;
    for (uint32_t i = 0; i < n; i++)
        ones += d[4 + i] == 0xFFFFFFFFu;
    printf("swept 0x%04x..0x%04x: %u addresses, %u read all-ones, %u answered\n", first, first + n - 1, n, ones, n - ones);
    for (uint32_t i = 0; i < n; i++) {
        if (d[4 + i] == 0xFFFFFFFFu)
            continue;
        printf("  %04x %-18s 0x%08x\n", first + i, regName(first + i), d[4 + i]);
    }
    return 0;
}

static int cmdCrumbs(void) {
    static uint32_t c[4 + CRUMB_N * 2];
    const uint32_t total = 4 + CRUMB_N * 2;
    for (uint32_t i = 0; i < total; i += 256)
        if (vramRead(CRUMB_OFF + i * 4, total - i > 256 ? 256 : total - i, c + i))
            return 1;
    if (c[0] != 0x4E435642u) {
        printf("no breadcrumbs (magic 0x%08x): VRAM did not keep them, or init never ran\n", c[0]);
        return 1;
    }
    const uint32_t n = c[1], shown = n < 48 ? n : 48;
    printf("%u accesses logged; last %u (the LAST line is the access in flight when it stopped):\n", n, shown);
    for (uint32_t k = n - shown; k < n; k++) {
        const uint32_t tag = c[4 + (k % CRUMB_N) * 2], v = c[4 + (k % CRUMB_N) * 2 + 1], lo = tag & 0x0FFFFFFF;
        switch (tag >> 28) {
        case 1: printf("  #%-5u read  0x%04x %-18s -> 0x%08x%s\n", k, lo, regName(lo), v, v == 0xEEEEEEEEu ? "  (never returned)" : ""); break;
        case 2: printf("  #%-5u write 0x%04x %-18s <- 0x%08x\n", k, lo, regName(lo), v); break;
        case 3: printf("  #%-5u SMU msg 0x%x arg 0x%x sent\n", k, lo, v); break;
        case 4: printf("  #%-5u SMU msg 0x%x answered 0x%x\n", k, lo, v); break;
        case 5:
            if (v)
                printf("  #%-5u ---- marker %u, value 0x%08x ----\n", k, lo, v);
            else
                printf("  #%-5u ---- stage %u reached ----\n", k, lo);
            break;
        default: printf("  #%-5u ? tag 0x%08x value 0x%08x\n", k, tag, v); break;
        }
    }
    return 0;
}

static uint64_t positiveNumber(const char *arg, int base) {
    char *end = NULL;
    errno = 0;
    const uint64_t value = strtoull(arg, &end, base);
    return errno || arg == end || *end || *arg < '0' || *arg > '9' ? 0 : value;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "check-firmware"))
        return cmdInit(argv[2], 0, 0, 1);
    if (argc < 2) {
        fprintf(stderr,
                "       vcnctl check-firmware <vcn_4_0_0.bin>\n"
                "usage: sudo vcnctl db <index> | sweep | crumbs | status | toc <toc.bin> | init [--static|--dpg|--dpg-pg|--dpg-sram] [--sram-full|--sram-pad N] [--sram-page N] [--sram-trim N] [--sram-abs ADDR] [--fw-type N] [--psp-ignore] [--fast-crumbs|--no-crumbs] [--stage 1..8] [--tmr <mc>] <vcn_4_0_0.bin> | ring [n] | fence [n] | decode-session | encode-session | decode-frame [output.nv12] | encode-frame [output.h264] | down\n"
                "  Build 60 and newer default to --dpg-sram. decode-session / encode-session submit create and close IBs.\n"
                "  init stages the firmware in the kext's fw_buf, loads it through the PSP, powers VCN up with\n"
                "  the SMU and boots the VCPU. --tmr skips the PSP load and uses a TMR address the boot script\n"
                "  already loaded (see /Users/Shared/gpu/fwaddr.log). ring submits VCN_ENC_CMD_END and checks\n"
                "  that the firmware moves the read pointer; fence submits a real VCN_ENC_CMD_FENCE packet and\n"
                "  waits for the firmware to write that value into the ring's WB slot. Nothing runs at boot.\n"
                "  --dpg starts VCN with the VCPU registers written through the DPG indirect port; --dpg-pg also enables\n"
                "  POWER_STATUS dynamic gating (gates the block dead on this chip).\n"
                "  --stage N stops init after N and powers VCN down: 1 PSP load, 2 SMU PowerUpVcn, 3 PGFSM planes on,\n"
                "  4 POWER_STATUS, 5 start-sequence registers, 6 VRAM block + fw_shared, 7 cache windows, 8 VCPU boot.\n");
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "run as root (the VCN selectors are root-only)\n");
        return 1;
    }
    if (openKext())
        return 1;

    int rc;
    if (!strcmp(argv[1], "db") && argc == 3) {
        uint64_t o[8] = {0};
        const uint32_t idx = (uint32_t)positiveNumber(argv[2], 0);
        rc = vcnOp(6, idx, 0, 0, 0, o) ? 1 : 0;
        if (!rc) printf("VCN ring doorbell index set to %u (%s)\n", idx, gBuild >= 61 ? "0 selects the default" : "0 disables");
    } else if (!strcmp(argv[1], "sweep")) {
        rc = cmdSweep();
    } else if (!strcmp(argv[1], "crumbs")) {
        rc = cmdCrumbs();
    } else if (!strcmp(argv[1], "status")) {
        rc = cmdStatus();
    } else if (!strcmp(argv[1], "init")) {
        if (argc >= 3 && !strcmp(argv[2], "--static")) {
            ++argv;
            --argc;
        } else if (argc >= 3 && (!strcmp(argv[2], "--dpg") || !strcmp(argv[2], "--dpg-pg") ||
                          !strcmp(argv[2], "--dpg-sram"))) {
            gDpg = 1;
            gDpgPg = !strcmp(argv[2], "--dpg-pg");
            gDpgSram = !strcmp(argv[2], "--dpg-sram");
            argv += 1;
            argc -= 1;
        } else if (gBuild >= 60) {
            gDpg = 1;
            gDpgSram = 1;
        }
        if (argc >= 4 && !strcmp(argv[2], "--sram-pad")) {
            gSramPad = (uint32_t)positiveNumber(argv[3], 0);
            if (!gSramPad || gSramPad > 8192u || (gSramPad & 7u)) {
                fprintf(stderr, "--sram-pad takes 8..8192, a multiple of 8\n");
                IOServiceClose(gConn);
                return 2;
            }
            gSramPadArg = gSramPad;
            argv += 2;
            argc -= 2;
        }
        if (argc >= 4 && !strcmp(argv[2], "--sram-abs")) {
            char *end = NULL;
            const unsigned long long a = strtoull(argv[3], &end, 0);
            if (end == argv[3] || *end || (a & 0xFFFu) || a > 0x0FFFFFFFull) {
                fprintf(stderr, "--sram-abs takes an aperture byte offset, 4 KB aligned\n");
                IOServiceClose(gConn);
                return 2;
            }
            gSramPadArg |= ((uint64_t)(a >> 12) & 0xFFFFull) << 48;
            argv += 2;
            argc -= 2;
        }
        if (argc >= 4 && !strcmp(argv[2], "--sram-trim")) {
            const uint32_t tr = (uint32_t)positiveNumber(argv[3], 0);
            if (!tr || tr > 255u) {
                fprintf(stderr, "--sram-trim takes 1..255\n");
                IOServiceClose(gConn);
                return 2;
            }
            gSramPadArg |= (uint64_t)tr << 32;
            argv += 2;
            argc -= 2;
        }
        if (argc >= 4 && !strcmp(argv[2], "--sram-page")) {
            const uint32_t pg = (uint32_t)positiveNumber(argv[3], 0);
            if (!pg || pg > 0xFFu) {
                fprintf(stderr, "--sram-page takes 1..255\n");
                IOServiceClose(gConn);
                return 2;
            }
            gSramPadArg |= (uint64_t)pg << 24;
            argv += 2;
            argc -= 2;
        }
        if (argc >= 3 && !strcmp(argv[2], "--psp-ignore")) {
            gSramPadArg |= 1ull << 20;
            argv += 1;
            argc -= 1;
        }
        if (argc >= 3 && !strcmp(argv[2], "--sram-full")) {
            gSramFull = 1;
            argv += 1;
            argc -= 1;
        }
        if (argc >= 4 && !strcmp(argv[2], "--fw-type")) {
            gFwType = (uint32_t)positiveNumber(argv[3], 0);
            if (!gFwType || gFwType > 0xFFu) {
                fprintf(stderr, "--fw-type takes 1..255\n");
                IOServiceClose(gConn);
                return 2;
            }
            argv += 2;
            argc -= 2;
        }
        if (argc >= 3 && (!strcmp(argv[2], "--fast-crumbs") || !strcmp(argv[2], "--no-crumbs"))) {
            gCrumbMode = !strcmp(argv[2], "--fast-crumbs") ? 1 : 2;
            argv += 1;
            argc -= 1;
        }
        if (argc >= 4 && !strcmp(argv[2], "--stage")) {
            gStage = (uint32_t)positiveNumber(argv[3], 10);
            if (gStage < 1 || gStage > 8) {
                fprintf(stderr, "--stage takes 1..8\n");
                IOServiceClose(gConn);
                return 2;
            }
            argv += 2;
            argc -= 2;
        }
        const char *path = NULL;
        uint64_t tmr = 0;
        if (argc == 3 && strcmp(argv[2], "--tmr")) {
            path = argv[2];
        } else if (argc == 5 && !strcmp(argv[2], "--tmr")) {
            tmr = positiveNumber(argv[3], 0);
            if (tmr)
                path = argv[4];
        }
        rc = path ? cmdInit(path, 0, tmr, 0) : (fprintf(stderr, "usage: vcnctl init [--tmr <mc>] <firmware>\n"), 2);
    } else if (argc == 2 && (!strcmp(argv[1], "decode-session") || !strcmp(argv[1], "encode-session"))) {
        rc = cmdCodecSession(!strcmp(argv[1], "encode-session"));
    } else if ((argc == 2 || argc == 3) && (!strcmp(argv[1], "encode-frame") || !strcmp(argv[1], "decode-frame"))) {
        rc = cmdCodecFrame(!strcmp(argv[1], "encode-frame"), argc == 3 ? argv[2] : NULL);
    } else if (!strcmp(argv[1], "toc") && argc == 3) {
        rc = cmdToc(argv[2]);
    } else if (!strcmp(argv[1], "ring") || !strcmp(argv[1], "fence")) {
        const uint64_t count = argc == 2 ? 1 : positiveNumber(argv[2], 10);
        if (argc > 3 || count == 0 || count > INT32_MAX) {
            fprintf(stderr, "%s needs a positive packet count\n", argv[1]);
            rc = 2;
        } else {
            rc = !strcmp(argv[1], "ring") ? cmdRing((int)count) : cmdFence((int)count);
        }
    } else if (!strcmp(argv[1], "down")) {
        rc = cmdDown();
    } else {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        rc = 2;
    }
    IOServiceClose(gConn);
    return rc;
}
