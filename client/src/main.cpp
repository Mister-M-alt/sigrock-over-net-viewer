// sonview entry point.
//   sonview                      -> GUI, default host 127.0.0.1
//   sonview --connect <host>     -> GUI, pre-filled host
//   sonview --unittest           -> headless store/pyramid tests (no SDL/GL)
//   sonview --selftest <host> [port] -> headless protocol round-trip (no SDL/GL)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "son_protocol.h"

namespace son {
int run_unittest();
int run_selftest(const std::string &host, uint16_t port);
int run_replay(const std::string &path, const std::string &export_base);
int run_gui(const std::string &default_host, bool autocapture, bool autoconnect);
}  // namespace son

int main(int argc, char **argv) {
    std::string host = "127.0.0.1";
    bool autocapture = false;
    bool autoconnect = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--autocapture")) autocapture = true;
    }
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--unittest")) {
            return son::run_unittest();
        } else if (!std::strcmp(argv[i], "--selftest")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "usage: sonview --selftest <host> [port]\n");
                return 2;
            }
            std::string h = argv[i + 1];
            uint16_t port = SON_DEFAULT_PORT;
            if (i + 2 < argc) port = (uint16_t)std::atoi(argv[i + 2]);
            return son::run_selftest(h, port);
        } else if (!std::strcmp(argv[i], "--replay")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                             "usage: sonview --replay <file.son> [--export <base>]\n");
                return 2;
            }
            std::string exp;
            for (int k = i + 2; k + 1 < argc; ++k)
                if (!std::strcmp(argv[k], "--export")) exp = argv[k + 1];
            return son::run_replay(argv[i + 1], exp);
        } else if (!std::strcmp(argv[i], "--connect")) {
            if (i + 1 < argc) {
                host = argv[++i];
                autoconnect = true;  // actually connect, don't just pre-fill
            }
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::printf(
                "sonview - sigrok-over-net viewer\n"
                "  sonview [--connect <host>]        GUI\n"
                "  sonview --unittest                headless self-tests\n"
                "  sonview --selftest <host> [port]  headless protocol test\n");
            return 0;
        }
    }
    return son::run_gui(host, autocapture, autoconnect && !autocapture);
}
