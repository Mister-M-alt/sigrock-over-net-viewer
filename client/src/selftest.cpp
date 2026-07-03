// Headless protocol self-test: connect, handshake, run one small triggered
// logic capture with a uart decoder, and summarise. No SDL/GL.
//   sonview --selftest <host> [port]
// Not runnable in the build sandbox (no server); it is written to match
// PROTOCOL.md exactly so it can be run against the real sond.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "model.h"
#include "net.h"
#include "son_protocol.h"
#include "son_wire.h"
#include "stores.h"
#include "zstd_util.h"

namespace son {

// Receive messages until one of type `want` arrives; print/skip others.
// Returns false on EOF or ERROR.
static bool recv_expect(Client &c, uint8_t want, son_msg &out) {
    for (;;) {
        if (!c.recv(out)) {
            std::printf("  [recv failed / connection closed]\n");
            return false;
        }
        if (out.hdr.type == SON_MSG_ERROR) {
            std::printf("  ERROR from server: %s\n", out.json().c_str());
            return false;
        }
        if (out.hdr.type == want) return true;
        std::printf("  [skipping msg type %u while waiting for %u]\n",
                    out.hdr.type, want);
    }
}

int run_selftest(const std::string &host, uint16_t port) {
    std::printf("== sonview --selftest %s:%u ==\n", host.c_str(), port);
    Client c;
    std::string err;
    if (!c.connect(host, port, err)) {
        std::printf("connect: %s\n", err.c_str());
        return 1;
    }
    std::printf("connected.\n");

    son_msg msg;

    // HELLO -> SERVER_INFO
    if (!c.send_json(SON_MSG_HELLO, R"({"client":"sonview","proto":1})")) return 1;
    if (!recv_expect(c, SON_MSG_SERVER_INFO, msg)) return 1;
    std::printf("SERVER_INFO: %s\n", msg.json().c_str());

    // SCAN_REQ -> SCAN_RESULT
    if (!c.send_empty(SON_MSG_SCAN_REQ)) return 1;
    if (!recv_expect(c, SON_MSG_SCAN_RESULT, msg)) return 1;
    std::vector<DeviceInfo> devices;
    if (!parse_scan_result(msg.json(), devices, err)) {
        std::printf("parse SCAN_RESULT: %s\n", err.c_str());
        return 1;
    }
    std::printf("devices (%zu):\n", devices.size());
    for (auto &d : devices)
        std::printf("  id=%d %s  channels=%zu samplerates=%zu triggers=%d\n",
                    d.id, d.label().c_str(), d.channels.size(),
                    d.samplerates.size(), (int)d.triggers);

    // pick the demo device
    const DeviceInfo *dev = nullptr;
    for (auto &d : devices)
        if (d.driver == "demo") { dev = &d; break; }
    if (!dev)
        for (auto &d : devices)
            if (d.model.find("demo") != std::string::npos ||
                d.driver.find("demo") != std::string::npos) { dev = &d; break; }
    if (!dev) {
        std::printf("no 'demo' device found; aborting.\n");
        return 1;
    }
    std::printf("selected device id=%d (%s)\n", dev->id, dev->label().c_str());

    // DECODERS_REQ -> DECODERS_LIST
    if (!c.send_empty(SON_MSG_DECODERS_REQ)) return 1;
    if (!recv_expect(c, SON_MSG_DECODERS_LIST, msg)) return 1;
    std::vector<DecoderMeta> decoders;
    if (!parse_decoders_list(msg.json(), decoders, err)) {
        std::printf("parse DECODERS_LIST: %s\n", err.c_str());
        return 1;
    }
    std::printf("decoders available: %zu\n", decoders.size());
    const DecoderMeta *uart = nullptr;
    for (auto &d : decoders)
        if (d.id == "uart") { uart = &d; break; }
    std::printf("uart decoder %s\n", uart ? "found" : "NOT found");

    // Build CONFIG: first 8 logic channels, samplerate 1 MHz, triggered 100k.
    std::vector<int> enabled;
    for (auto &ch : dev->channels) {
        if (ch.type == "logic") {
            enabled.push_back(ch.index);
            if (enabled.size() >= 8) break;
        }
    }
    if (enabled.empty()) {
        std::printf("device has no logic channels; aborting.\n");
        return 1;
    }
    std::printf("enabling %zu logic channels\n", enabled.size());

    std::vector<std::pair<int, std::string>> triggers;
    triggers.push_back({enabled[0], "rising"});  // trigger on ch0 rising

    std::vector<DecoderInstance> insts;
    if (uart) {
        DecoderInstance inst;
        inst.meta = *uart;
        init_decoder_defaults(inst);
        int rx = enabled.size() > 1 ? enabled[1] : enabled[0];
        // Map whichever of rx/tx roles exist (uart typically has both).
        for (auto &dc : uart->channels) {
            if (dc.id == "rx") inst.ch_map["rx"] = rx;
        }
        if (!inst.ch_map.count("rx") && !uart->channels.empty())
            inst.ch_map[uart->channels[0].id] = rx;
        inst.opts["baudrate"] = 115200;  // explicit; also in defaults
        insts.push_back(inst);
    }

    std::string cfg = build_config(dev->id, 1000000, enabled, "triggered", 100000,
                                   0, triggers, insts);
    std::printf("CONFIG: %s\n", cfg.c_str());
    if (!c.send_json(SON_MSG_CONFIG, cfg)) return 1;
    if (!c.send_empty(SON_MSG_START)) return 1;

    // Consume the capture.
    LogicStore logic;
    SessionMeta meta;
    bool have_meta = false;
    std::vector<int> tracked_bits;
    uint64_t logic_samples = 0;
    std::vector<std::pair<uint32_t, uint64_t>> analog_counts;  // channel_id -> samples
    uint64_t ann_count = 0;

    bool done = false;
    while (!done) {
        if (!c.recv(msg)) {
            std::printf("  [connection closed before CAPTURE_END]\n");
            return 1;
        }
        if (!maybe_decompress(msg)) {
            std::printf("  zstd decompress failed\n");
            return 1;
        }
        switch (msg.hdr.type) {
            case SON_MSG_SESSION_META: {
                if (!parse_session_meta(msg.json(), meta, err)) {
                    std::printf("parse SESSION_META: %s\n", err.c_str());
                    return 1;
                }
                logic.reset(meta.unitsize ? meta.unitsize : 1);
                tracked_bits.clear();
                for (auto &ch : meta.channels)
                    if (ch.type == "logic" && ch.bit >= 0) tracked_bits.push_back(ch.bit);
                logic.set_tracked_bits(tracked_bits);
                have_meta = true;
                std::printf("SESSION_META: samplerate=%llu unitsize=%u total=%llu "
                            "channels=%zu\n",
                            (unsigned long long)meta.samplerate, meta.unitsize,
                            (unsigned long long)meta.total_samples,
                            meta.channels.size());
                break;
            }
            case SON_MSG_DECODER_INFO: {
                std::vector<DecoderRowInfo> ri;
                if (parse_decoder_info(msg.json(), ri, err))
                    std::printf("DECODER_INFO: %zu decoder(s)\n", ri.size());
                break;
            }
            case SON_MSG_DATA_LOGIC: {
                if (msg.payload.size() < sizeof(son_logic_hdr)) break;
                son_logic_hdr h;
                std::memcpy(&h, msg.payload.data(), sizeof(h));
                const uint8_t *bytes = msg.payload.data() + sizeof(son_logic_hdr);
                size_t need = (size_t)h.sample_count * h.unitsize;
                if (msg.payload.size() < sizeof(son_logic_hdr) + need) {
                    std::printf("  short DATA_LOGIC payload\n");
                    break;
                }
                if (have_meta && h.unitsize == meta.unitsize) {
                    bool disc = (msg.hdr.flags & SON_FLAG_DISCONTINUITY) != 0;
                    logic.append(h.start_sample, h.sample_count, bytes, disc);
                }
                logic_samples += h.sample_count;
                break;
            }
            case SON_MSG_DATA_ANALOG: {
                if (msg.payload.size() < sizeof(son_analog_hdr)) break;
                son_analog_hdr h;
                std::memcpy(&h, msg.payload.data(), sizeof(h));
                bool found = false;
                for (auto &p : analog_counts)
                    if (p.first == h.channel_id) { p.second += h.sample_count; found = true; }
                if (!found) analog_counts.push_back({h.channel_id, h.sample_count});
                break;
            }
            case SON_MSG_ANN_BATCH: {
                if (msg.payload.size() < sizeof(son_ann_batch_hdr)) break;
                son_ann_batch_hdr bh;
                std::memcpy(&bh, msg.payload.data(), sizeof(bh));
                size_t off = sizeof(son_ann_batch_hdr);
                for (uint16_t i = 0; i < bh.count; ++i) {
                    if (off + sizeof(son_ann) > msg.payload.size()) break;
                    son_ann a;
                    std::memcpy(&a, msg.payload.data() + off, sizeof(a));
                    off += sizeof(son_ann);
                    for (uint16_t t = 0; t < a.n_texts; ++t) {
                        if (off + 2 > msg.payload.size()) { off = msg.payload.size(); break; }
                        uint16_t len;
                        std::memcpy(&len, msg.payload.data() + off, 2);
                        off += 2 + len;
                        if (off > msg.payload.size()) { off = msg.payload.size(); break; }
                    }
                    ++ann_count;
                }
                break;
            }
            case SON_MSG_CAPTURE_END: {
                std::printf("CAPTURE_END: %s\n", msg.json().c_str());
                done = true;
                break;
            }
            case SON_MSG_ERROR: {
                std::printf("ERROR: %s\n", msg.json().c_str());
                return 1;
            }
            default:
                std::printf("  [ignoring msg type %u]\n", msg.hdr.type);
                break;
        }
    }

    // Per-channel transition counts from the pyramid.
    uint64_t total_transitions = 0;
    std::printf("\n--- summary ---\n");
    std::printf("logic samples received: %llu (per enabled logic channel)\n",
                (unsigned long long)logic_samples);
    for (auto &ch : meta.channels) {
        if (ch.type != "logic" || ch.bit < 0) continue;
        std::vector<Edge> e;
        uint8_t init;
        logic.walk(ch.bit, 0, logic.count(), 1.0, init, e, nullptr);
        total_transitions += e.size();
        std::printf("  ch %-3d %-8s bit=%d transitions=%zu\n", ch.index,
                    ch.name.c_str(), ch.bit, e.size());
    }
    std::printf("total transitions: %llu\n", (unsigned long long)total_transitions);
    for (auto &p : analog_counts)
        std::printf("analog channel %u: %llu samples\n", p.first,
                    (unsigned long long)p.second);
    std::printf("annotations: %llu\n", (unsigned long long)ann_count);
    return 0;
}

}  // namespace son
