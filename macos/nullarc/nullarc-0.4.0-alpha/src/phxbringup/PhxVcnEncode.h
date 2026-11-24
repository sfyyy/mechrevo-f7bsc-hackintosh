// SPDX-License-Identifier: MIT
// VCN 4 unified-queue encoder ring self-test. Matches the create/destroy IBs in
// amdgpu_vcn_enc_get_{create,destroy}_msg (drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c).
#ifndef PHX_VCN_ENCODE_H
#define PHX_VCN_ENCODE_H

#include <stdint.h>

namespace phx_vcn {

constexpr uint32_t kEncodeIbDw = 21; // 4 checksum + 4 engine + 6 session + 5 task + 2 op

// The caller owns the zeroed, page-aligned 128 KiB session context through both fences.
// Firmware ring diagnostics use handle 1 and ABI operations 0x08000001/2.
inline uint32_t buildEncodeSessionIb(uint32_t *words, uint64_t contextMc, bool close) {
    words[0] = 16;
    words[1] = 0x30000002; // checksum package
    words[2] = 0;
    words[3] = kEncodeIbDw - 4;
    words[4] = 16;
    words[5] = 0x30000001; // engine-info package
    words[6] = 2; // encode
    words[7] = words[3] * sizeof(uint32_t);

    words[8] = 24;
    words[9] = 1; // session info
    words[10] = 1; // session handle
    words[11] = static_cast<uint32_t>(contextMc >> 32);
    words[12] = static_cast<uint32_t>(contextMc);
    words[13] = 0;

    words[14] = 20;
    words[15] = 2; // task info
    words[16] = 28; // task info + operation, in bytes
    words[17] = 0;
    words[18] = 0;
    words[19] = 8;
    words[20] = close ? 0x08000002u : 0x08000001u;

    for (uint32_t i = 4; i < kEncodeIbDw; ++i)
        words[2] += words[i];
    return kEncodeIbDw;
}

} // namespace phx_vcn

#endif // PHX_VCN_ENCODE_H
