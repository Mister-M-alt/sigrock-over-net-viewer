// Shared client-side helper: decompress a data-plane frame in place when it
// carries SON_FLAG_ZSTD (used by both the GUI RX path and the headless selftest).
#pragma once
#include <zstd.h>

#include <cstdint>
#include <vector>

#include "son_protocol.h"
#include "son_wire.h"

namespace son {

inline bool maybe_decompress(son_msg &msg) {
    if (!(msg.hdr.flags & SON_FLAG_ZSTD)) return true;
    unsigned long long dsz = ZSTD_getFrameContentSize(msg.payload.data(), msg.payload.size());
    if (dsz == ZSTD_CONTENTSIZE_ERROR || dsz == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    std::vector<uint8_t> out((size_t)dsz);
    size_t r = ZSTD_decompress(out.data(), out.size(), msg.payload.data(), msg.payload.size());
    if (ZSTD_isError(r) || r != out.size()) return false;
    msg.payload.swap(out);
    msg.hdr.flags &= (uint16_t)~SON_FLAG_ZSTD;
    msg.hdr.length = (uint32_t)msg.payload.size();
    return true;
}

}  // namespace son
