#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace son {

// Non-blocking connect with a poll() deadline; returns 0 on success, else errno.
static int connect_timeout(int fd, const struct sockaddr *addr, socklen_t len,
                           int timeout_ms) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, addr, len);
    int result = 0;
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            result = errno;
        } else {
            struct pollfd p = {fd, POLLOUT, 0};
            rc = ::poll(&p, 1, timeout_ms);
            if (rc == 0) {
                result = ETIMEDOUT;
            } else if (rc < 0) {
                result = errno;
            } else {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                result = soerr;
            }
        }
    }
    ::fcntl(fd, F_SETFL, flags);  // back to blocking for son_wire I/O
    return result;
}

bool Client::connect(const std::string &host, uint16_t port, std::string &err,
                     int timeout_ms) {
    close();
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    std::snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo *res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), portstr, &hints, &res);
    if (gai != 0) {
        err = std::string("getaddrinfo: ") + gai_strerror(gai);
        return false;
    }

    int fd = -1;
    int last_err = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int ce = connect_timeout(fd, ai->ai_addr, ai->ai_addrlen, timeout_ms);
        if (ce == 0) break;
        last_err = ce;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
        err = std::strerror(last_err);
        return false;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fd_ = fd;
    return true;
}

void Client::close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace son
