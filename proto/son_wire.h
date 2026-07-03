#ifndef SON_WIRE_H
#define SON_WIRE_H
// Blocking framed-message I/O over a TCP socket fd, shared by sond and sonview.
// Header-only. Every message = son_hdr (16 bytes LE) + length bytes of payload.
#include "son_protocol.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <errno.h>

struct son_msg {
    son_hdr hdr;
    std::vector<uint8_t> payload;
    std::string json() const { return std::string((const char *)payload.data(), payload.size()); }
};

// Write exactly n bytes; returns false on error/EOF.
static inline bool son_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

// Read exactly n bytes; returns false on error/EOF.
static inline bool son_read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

// Send a framed message. For control messages pass stream_id 0.
static inline bool son_send(int fd, uint8_t type, uint16_t flags, uint32_t stream_id,
                            const void *payload, uint32_t len)
{
    son_hdr h;
    h.magic = SON_MAGIC; h.version = SON_PROTO_VERSION; h.type = type;
    h.flags = flags; h.stream_id = stream_id; h.length = len;
    if (!son_write_all(fd, &h, sizeof(h))) return false;
    if (len && !son_write_all(fd, payload, len)) return false;
    return true;
}

static inline bool son_send_json(int fd, uint8_t type, const std::string &j)
{
    return son_send(fd, type, 0, 0, j.data(), (uint32_t)j.size());
}

// Send a binary data-plane message: a fixed sub-header followed by a data blob.
static inline bool son_send_data(int fd, uint8_t type, uint16_t flags, uint32_t stream_id,
                                 const void *subhdr, uint32_t subhdr_len,
                                 const void *data, uint32_t data_len)
{
    son_hdr h;
    h.magic = SON_MAGIC; h.version = SON_PROTO_VERSION; h.type = type;
    h.flags = flags; h.stream_id = stream_id; h.length = subhdr_len + data_len;
    if (!son_write_all(fd, &h, sizeof(h))) return false;
    if (subhdr_len && !son_write_all(fd, subhdr, subhdr_len)) return false;
    if (data_len && !son_write_all(fd, data, data_len)) return false;
    return true;
}

// Receive one full framed message. Returns false on error/EOF or bad magic.
static inline bool son_recv(int fd, son_msg &out)
{
    if (!son_read_all(fd, &out.hdr, sizeof(out.hdr))) return false;
    if (out.hdr.magic != SON_MAGIC) return false;
    out.payload.resize(out.hdr.length);
    if (out.hdr.length && !son_read_all(fd, out.payload.data(), out.hdr.length)) return false;
    return true;
}

#endif // SON_WIRE_H
