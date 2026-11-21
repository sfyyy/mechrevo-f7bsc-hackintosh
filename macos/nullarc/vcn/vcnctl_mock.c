/* Fake IOKit transport for testing the actual vcnctl binary without root or MMIO. */
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool hello;
static size_t staged;
static FILE *expected;

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
        out[1] = 37;
        hello = true;
        return KERN_SUCCESS;
    }
    if (!hello)
        return kIOReturnNotReady;
    assert(selector == 120 && inCount == 5 && *outCount == 8);
    memset(out, 0, 8 * sizeof(*out));
    if (in[0] == 2) {
        assert(expected && in[1] == 0 && in[2] == staged && staged > 32 && fgetc(expected) == EOF);
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
