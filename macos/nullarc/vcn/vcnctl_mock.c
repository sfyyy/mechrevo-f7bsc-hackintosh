/* Fake IOKit transport for testing the actual vcnctl binary without root or MMIO. */
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool hello;
static unsigned int build;
static size_t staged;
static FILE *expected;
static uint8_t *frame;
static size_t capacity;
static bool mapped;

uid_t geteuid(void) { return 0; }

io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching) {
    assert(port == kIOMainPortDefault && matching);
    CFRelease(matching);
    return 1;
}

kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connection) {
    assert(service == 1 && task == mach_task_self() && type == 0x4E34384E);
    *connection = 2;
    return KERN_SUCCESS;
}

kern_return_t IOObjectRelease(io_object_t object) {
    assert(object == 1);
    return KERN_SUCCESS;
}

kern_return_t IOServiceClose(io_connect_t connection) {
    assert(connection == 2);
    assert(!frame && !mapped);
    if (expected)
        fclose(expected);
    return KERN_SUCCESS;
}

kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *in,
                                         uint32_t inCount, uint64_t *out, uint32_t *outCount) {
    assert(connection == 2);
    if (selector == 0) {
        assert(!hello && inCount == 2 && in[0] == 1 && in[1] == 0 && *outCount == 4);
        memset(out, 0, 4 * sizeof(*out));
        out[0] = 1;
        const char *configured = getenv("VCN_TEST_BUILD");
        build = configured ? (unsigned int)strtoul(configured, NULL, 10) : 37;
        out[1] = build;
        hello = true;
        return KERN_SUCCESS;
    }
    if (!hello)
        return kIOReturnNotReady;
    if (selector == 4) {
        assert(inCount == 1 && in[0] == 3 && !out && !outCount && frame && !mapped);
        free(frame);
        frame = NULL;
        return KERN_SUCCESS;
    }
    assert(selector == 120 && inCount == (build <= 38 ? 5u : 6u) && *outCount == 8);
    memset(out, 0, 8 * sizeof(*out));
    if ((in[0] & 0xff) == 2) {
        assert(expected && in[1] == 0 && in[2] == staged && staged > 32 && fgetc(expected) == EOF);
        const bool indirect = build >= 60 && !getenv("VCN_TEST_STATIC");
        assert(in[0] == (indirect ? 0x140002u : 2u));
    } else if (in[0] == 8 || in[0] == 9) {
        assert(build >= 60);
        out[1] = 2;
        out[2] = 17;
        out[3] = getenv("VCN_TEST_STALE_FENCE") ? 16 : 17;
    } else if (in[0] == 10 || in[0] == 11) {
        const bool encode = in[0] == 11;
        assert(build >= 62 && mapped && in[1] == 3 && !in[2] && !in[3] && !in[4] && !in[5]);
        assert(capacity == (encode ? 28672u : 737280u));
        out[1] = getenv("VCN_TEST_BAD_PHASE") ? 2 : 3;
        out[2] = 19;
        out[3] = getenv("VCN_TEST_STALE_FENCE") ? 18 : 19;
        if (encode) {
            const char *path = getenv("VCN_TEST_FRAME");
            assert(path);
            FILE *reference = fopen(path, "rb");
            assert(reference);
            out[6] = out[7] = fread(frame, 1, capacity, reference);
            assert(out[6] && feof(reference));
            fclose(reference);
        } else {
            out[6] = capacity;
            out[7] = 0x020345d8;
            memset(frame, (int)(out[7] / capacity), capacity);
            for (size_t i = 0; i < out[7] % capacity; ++i) ++frame[i];
        }
        if (getenv("VCN_TEST_BAD_PAYLOAD")) frame[0] ^= 1;
        if (getenv("VCN_TEST_BAD_LENGTH")) out[6] = capacity + 1;
    } else {
        assert(in[0] == 1);
    }
    return KERN_SUCCESS;
}

kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *in, uint32_t inCount,
                                   const void *input, size_t bytes, uint64_t *out, uint32_t *outCount,
                                   void *output, size_t *outputSize) {
    if (!hello)
        return kIOReturnNotReady;
    if (selector == 3) {
        const uint64_t *request = input;
        assert(build >= 62 && connection == 2 && !frame && !in && !inCount && bytes == 32 && *outCount == 4);
        assert(request[1] == 4096 && request[2] == 4 && !request[3]);
        capacity = request[0];
        assert(capacity == 28672 || capacity == 737280);
        frame = calloc(1, capacity);
        assert(frame);
        out[0] = 3; out[1] = capacity; out[2] = 4; out[3] = 3;
        return KERN_SUCCESS;
    }
    assert(connection == 2 && selector == 104 && inCount == 1 && in[0] == staged);
    assert(!out && !outCount && !output && !outputSize);
    if (!expected) {
        const char *path = getenv("VCN_TEST_FIRMWARE");
        assert(path);
        expected = fopen(path, "rb");
        assert(expected);
    }
    uint8_t buffer[4096];
    assert(bytes > 0 && bytes <= sizeof(buffer));
    assert(fread(buffer, 1, bytes, expected) == bytes && memcmp(input, buffer, bytes) == 0);
    staged += bytes;
    return KERN_SUCCESS;
}

kern_return_t IOConnectMapMemory64(io_connect_t connection, uint32_t type, task_port_t task,
                                   mach_vm_address_t *address, mach_vm_size_t *size, IOOptionBits options) {
    assert(connection == 2 && type == 3 && frame && !mapped && task == mach_task_self() && options == kIOMapAnywhere);
    mapped = true;
    *address = (uintptr_t)frame;
    *size = capacity;
    return KERN_SUCCESS;
}

kern_return_t IOConnectUnmapMemory64(io_connect_t connection, uint32_t type, task_port_t task,
                                     mach_vm_address_t address) {
    assert(connection == 2 && type == 3 && mapped && task == mach_task_self() && address == (uintptr_t)frame);
    mapped = false;
    return KERN_SUCCESS;
}
