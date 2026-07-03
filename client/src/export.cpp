// Capture export: VCD (universal) and sigrok .sr / srzip (PulseView,
// sigrok-cli). Logic channels only — analog viewers use the .son format.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"

namespace son {

// ---- minimal store-only ZIP writer (for srzip) ------------------------------
namespace {

uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct ZipWriter {
    FILE *f = nullptr;
    struct Entry {
        std::string name;
        uint32_t crc, size, offset;
    };
    std::vector<Entry> entries;

    bool open(const std::string &path) { return (f = std::fopen(path.c_str(), "wb")) != nullptr; }
    void u16(uint16_t v) { std::fwrite(&v, 2, 1, f); }
    void u32(uint32_t v) { std::fwrite(&v, 4, 1, f); }

    void add(const std::string &name, const uint8_t *data, size_t n) {
        Entry e;
        e.name = name;
        e.crc = crc32_of(data, n);
        e.size = (uint32_t)n;
        e.offset = (uint32_t)std::ftell(f);
        u32(0x04034b50);           // local file header
        u16(20); u16(0); u16(0);   // version, flags, method=store
        u16(0); u16(0);            // mod time/date
        u32(e.crc); u32(e.size); u32(e.size);
        u16((uint16_t)name.size()); u16(0);
        std::fwrite(name.data(), 1, name.size(), f);
        std::fwrite(data, 1, n, f);
        entries.push_back(std::move(e));
    }

    void close() {
        uint32_t cd_off = (uint32_t)std::ftell(f);
        for (auto &e : entries) {
            u32(0x02014b50);            // central directory record
            u16(20); u16(20); u16(0); u16(0);
            u16(0); u16(0);             // time/date
            u32(e.crc); u32(e.size); u32(e.size);
            u16((uint16_t)e.name.size()); u16(0); u16(0);
            u16(0); u16(0); u32(0);     // disk, int attrs, ext attrs
            u32(e.offset);
            std::fwrite(e.name.data(), 1, e.name.size(), f);
        }
        uint32_t cd_size = (uint32_t)std::ftell(f) - cd_off;
        u32(0x06054b50);                // end of central directory
        u16(0); u16(0);
        u16((uint16_t)entries.size()); u16((uint16_t)entries.size());
        u32(cd_size); u32(cd_off);
        u16(0);
        std::fclose(f);
        f = nullptr;
    }
};

std::string samplerate_str(uint64_t hz) {
    char b[48];
    if (hz >= 1000000000ULL && hz % 1000000000ULL == 0)
        std::snprintf(b, sizeof(b), "%llu GHz", (unsigned long long)(hz / 1000000000ULL));
    else if (hz >= 1000000ULL && hz % 1000000ULL == 0)
        std::snprintf(b, sizeof(b), "%llu MHz", (unsigned long long)(hz / 1000000ULL));
    else if (hz >= 1000ULL && hz % 1000ULL == 0)
        std::snprintf(b, sizeof(b), "%llu kHz", (unsigned long long)(hz / 1000ULL));
    else
        std::snprintf(b, sizeof(b), "%llu Hz", (unsigned long long)hz);
    return b;
}

}  // namespace

// sigrok session archive: readable by PulseView / sigrok-cli (-i file.sr).
bool App::export_sr(const std::string &path, std::string &err) {
    SessionMeta m = meta_copy();
    uint64_t fl = logic().first_live(), total = logic().count();
    if (total <= fl) { err = "no logic data captured"; return false; }
    int unitsize = logic().unitsize() ? logic().unitsize() : 1;

    std::vector<const ChannelInfo *> logic_chs;
    for (auto &ch : m.channels)
        if (ch.type == "logic" && ch.bit >= 0) logic_chs.push_back(&ch);
    if (logic_chs.empty()) { err = "no logic channels"; return false; }

    ZipWriter z;
    if (!z.open(path)) { err = "cannot open " + path; return false; }
    z.add("version", (const uint8_t *)"2", 1);

    std::string meta_ini = "[global]\nsigrok version=0.5.2\n\n[device 1]\ncapturefile=logic-1\n";
    meta_ini += "total probes=" + std::to_string(logic_chs.size()) + "\n";
    meta_ini += "samplerate=" + samplerate_str(m.samplerate) + "\n";
    for (size_t i = 0; i < logic_chs.size(); ++i)
        meta_ini += "probe" + std::to_string(i + 1) + "=" +
                    chan_name(logic_chs[i]->index, logic_chs[i]->name) + "\n";
    meta_ini += "unitsize=" + std::to_string(unitsize) + "\n";
    z.add("metadata", (const uint8_t *)meta_ini.data(), meta_ini.size());

    const uint32_t CHUNK = 4 * 1024 * 1024 / unitsize;  // ~4 MB per zip entry
    std::vector<uint8_t> buf;
    int idx = 1;
    for (uint64_t s = fl; s < total; s += CHUNK) {
        uint32_t n = (uint32_t)std::min<uint64_t>(CHUNK, total - s);
        buf.resize((size_t)n * unitsize);
        logic().copy_raw(s, n, buf.data());
        z.add("logic-1-" + std::to_string(idx++), buf.data(), buf.size());
    }
    z.close();
    return true;
}

// Value-change dump: logic channels, ps timescale (exact for any samplerate).
bool App::export_vcd(const std::string &path, std::string &err) {
    SessionMeta m = meta_copy();
    uint64_t fl = logic().first_live(), total = logic().count();
    if (total <= fl) { err = "no logic data captured"; return false; }
    if (m.samplerate == 0) { err = "unknown samplerate"; return false; }

    std::vector<const ChannelInfo *> chs;
    for (auto &ch : m.channels)
        if (ch.type == "logic" && ch.bit >= 0) chs.push_back(&ch);
    if (chs.empty()) { err = "no logic channels"; return false; }

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open " + path; return false; }

    // Coarsest power-of-10 timescale that exactly divides the sample period —
    // keeps sigrok's VCD input from synthesizing millions of sub-samples.
    uint64_t ps_per_sample = (uint64_t)(1e12 / (double)m.samplerate + 0.5);
    static const char *SCALE[] = {"1 ps",  "10 ps",  "100 ps", "1 ns",  "10 ns",
                                  "100 ns", "1 us",  "10 us",  "100 us", "1 ms",
                                  "10 ms", "100 ms", "1 s"};
    int scale = 0;
    uint64_t div = 1;
    while (scale < 12 && ps_per_sample % (div * 10) == 0) {
        div *= 10;
        ++scale;
    }
    ps_per_sample /= div;  // now in timescale units
    std::fprintf(f, "$version sonview $end\n$timescale %s $end\n", SCALE[scale]);
    std::fprintf(f, "$scope module sonview $end\n");
    for (size_t i = 0; i < chs.size(); ++i) {
        std::string nm = chan_name(chs[i]->index, chs[i]->name);
        for (auto &c : nm)
            if (c == ' ' || c == '\t') c = '_';
        std::fprintf(f, "$var wire 1 %c %s $end\n", (char)('!' + i), nm.c_str());
    }
    std::fprintf(f, "$upscope $end\n$enddefinitions $end\n");

    // collect exact edges per channel, then k-way merge by sample
    std::vector<std::vector<Edge>> edges(chs.size());
    std::vector<uint8_t> init(chs.size());
    for (size_t i = 0; i < chs.size(); ++i)
        logic().walk(chs[i]->bit, fl, total, 1.0, init[i], edges[i], nullptr);

    std::fprintf(f, "#%llu\n", (unsigned long long)(fl * ps_per_sample));
    for (size_t i = 0; i < chs.size(); ++i)
        std::fprintf(f, "%d%c\n", (int)init[i], (char)('!' + i));

    std::vector<size_t> pos(chs.size(), 0);
    for (;;) {
        uint64_t best = UINT64_MAX;
        for (size_t i = 0; i < chs.size(); ++i)
            if (pos[i] < edges[i].size() && edges[i][pos[i]].sample < best)
                best = edges[i][pos[i]].sample;
        if (best == UINT64_MAX) break;
        std::fprintf(f, "#%llu\n", (unsigned long long)(best * ps_per_sample));
        for (size_t i = 0; i < chs.size(); ++i)
            while (pos[i] < edges[i].size() && edges[i][pos[i]].sample == best) {
                std::fprintf(f, "%d%c\n", (int)edges[i][pos[i]].value, (char)('!' + i));
                ++pos[i];
            }
    }
    std::fprintf(f, "#%llu\n", (unsigned long long)(total * ps_per_sample));
    std::fclose(f);
    return true;
}

}  // namespace son
