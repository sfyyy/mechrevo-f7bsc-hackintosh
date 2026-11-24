#pragma once

#include <stdint.h>

namespace phx_vcn {

// Use aligned, explicit 64-bit device accesses through BAR0.
inline void readApertureRow(uint8_t *dst, const volatile uint8_t *src, uint32_t size) {
    while (size >= sizeof(uint64_t)) {
        const uint64_t word = *reinterpret_cast<const volatile uint64_t *>(src);
        __builtin_memcpy(dst, &word, sizeof(word));
        src += sizeof(word);
        dst += sizeof(word);
        size -= sizeof(word);
    }
    while (size--) *dst++ = *src++;
}

inline void writeAperture(volatile uint8_t *dst, const uint8_t *src, uint32_t size) {
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, src, sizeof(word));
        *reinterpret_cast<volatile uint64_t *>(dst) = word;
        src += sizeof(word);
        dst += sizeof(word);
        size -= sizeof(word);
    }
    while (size--) *dst++ = *src++;
}

inline void clearAperture(volatile uint8_t *dst, uint32_t size) {
    while (size && (reinterpret_cast<uintptr_t>(dst) & 7u)) {
        *dst++ = 0;
        --size;
    }
    while (size >= sizeof(uint64_t)) {
        *reinterpret_cast<volatile uint64_t *>(dst) = 0;
        dst += sizeof(uint64_t);
        size -= sizeof(uint64_t);
    }
    while (size--) *dst++ = 0;
}

} // namespace phx_vcn
