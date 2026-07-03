// TCP client for the sigrok-over-net protocol. Thin wrapper over son_wire.h.
#pragma once
#include <cstdint>
#include <string>

#include "son_protocol.h"
#include "son_wire.h"

namespace son {
// (message types, incl. DECODERS_REQ/LIST, come from son_protocol.h)

class Client {
public:
    Client() = default;
    ~Client() { close(); }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Connects with a bounded timeout so a wrong IP / powered-off host can
    // never hang the caller for the kernel's multi-minute SYN timeout.
    bool connect(const std::string &host, uint16_t port, std::string &err,
                 int timeout_ms = 4000);
    void close();
    bool connected() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    bool send_json(uint8_t type, const std::string &json) {
        return fd_ >= 0 && son_send_json(fd_, type, json);
    }
    bool send_empty(uint8_t type) { return send_json(type, "{}"); }

    // Blocking receive of one framed message.
    bool recv(son_msg &out) { return fd_ >= 0 && son_recv(fd_, out); }

private:
    int fd_ = -1;
};

}  // namespace son
