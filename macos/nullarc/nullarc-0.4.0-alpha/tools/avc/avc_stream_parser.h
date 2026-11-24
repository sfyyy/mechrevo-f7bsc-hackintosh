// SPDX-License-Identifier: MIT
#ifndef PHX_HOST_AVC_STREAM_PARSER_H
#define PHX_HOST_AVC_STREAM_PARSER_H

#include "../../src/phxbringup/PhxVcnAvcStream.h"
#include "avd_h264.h"

namespace phx_vcn {

struct AvcVclRange {
    size_t offset, length;
};

// Feed one complete Annex B access unit per call, starting with an IDR
// containing SPS and PPS. A rejected access unit leaves the parser and outputs
// intact. Reset after a seek or loss of reference pictures.
class AvcStreamParser {
public:
    bool parse(const uint8_t *data, size_t size, AvcStreamSequence &sequence,
               AvcStreamPicture &picture, uint32_t &sliceCount, AvcVclRange *vcl = nullptr);
    void reset() { *this = {}; }

private:
    PhoenixH264SPS sps_{};
    PhoenixH264PPS pps_{};
    AvcReference references_[kAvcMaxReferences]{};
    uint8_t referenceCount_ = 0;
    AvcReference previousReference_{};
    bool haveReference_ = false;
};

} // namespace phx_vcn
#endif
