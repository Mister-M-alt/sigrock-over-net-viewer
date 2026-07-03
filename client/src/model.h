// Control-plane data model: parse SCAN_RESULT / DECODERS_LIST / SESSION_META /
// DECODER_INFO and build CONFIG. Shared by the GUI and the --selftest path so
// both speak exactly the same JSON (PROTOCOL.md).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

namespace son {
using json = nlohmann::json;

struct ChannelInfo {
    int index = 0;
    std::string name;
    std::string type = "logic";  // "logic" | "analog"
    int bit = -1;                // position in the logic word (from SESSION_META)
    // GUI capture selections:
    bool enabled = false;
    std::string trigger = "none";  // none|0|1|rising|falling|edge
};

struct DeviceInfo {
    int id = -1;
    std::string driver, vendor, model, conn;
    std::vector<uint64_t> samplerates;
    bool triggers = false;
    std::vector<ChannelInfo> channels;
    std::string label() const {
        std::string s = vendor + " " + model;
        if (!driver.empty()) s += " [" + driver + "]";
        if (!conn.empty()) s += " (" + conn + ")";
        return s;
    }
};

struct DecOption {
    enum Kind { ENUM, INT, STR };
    std::string id, desc;
    Kind kind = STR;
    std::vector<std::string> values;  // for ENUM
    json def;                         // default value
};

struct DecChannel {
    std::string id, name, desc;
    bool required = false;
};

struct DecoderMeta {
    std::string id, name, longname;
    std::vector<std::string> tags;
    std::vector<DecChannel> channels;
    std::vector<DecOption> options;
};

// A decoder chosen for this capture, with role->channel mapping and options.
struct DecoderInstance {
    DecoderMeta meta;
    std::map<std::string, int> ch_map;  // role id -> captured channel index (-1)
    json opts = json::object();         // option id -> current value
    int stack_id = -1;                  // assigned by DECODER_INFO
};

// Parsers (return false + err on malformed input).
bool parse_scan_result(const std::string &js, std::vector<DeviceInfo> &out, std::string &err);
bool parse_decoders_list(const std::string &js, std::vector<DecoderMeta> &out, std::string &err);

// SESSION_META: fills samplerate/unitsize/total and per-channel bit positions.
struct SessionMeta {
    uint64_t samplerate = 0;
    uint8_t unitsize = 1;
    uint64_t total_samples = 0;  // 0 => continuous
    std::vector<ChannelInfo> channels;  // includes bit
};
bool parse_session_meta(const std::string &js, SessionMeta &out, std::string &err);

// DECODER_INFO: map (id -> stack_id) and (stack_id,row) names.
struct DecoderRowInfo {
    uint32_t stack_id = 0;
    std::string id;
    std::vector<std::pair<int, std::string>> rows;  // row_id -> name
};
bool parse_decoder_info(const std::string &js, std::vector<DecoderRowInfo> &out, std::string &err);

// Initialise an instance's options object from the metadata defaults.
void init_decoder_defaults(DecoderInstance &inst);

// Decoder instances -> CONFIG/DECODE_REQ "decoders" JSON array.
json decoders_to_json(const std::vector<DecoderInstance> &decoders);

// Build the CONFIG JSON payload string. capture_ratio = pre-trigger %, 0 = none.
std::string build_config(int device_id, uint64_t samplerate,
                         const std::vector<int> &channels, const std::string &mode,
                         uint64_t limit_samples, int capture_ratio,
                         const std::vector<std::pair<int, std::string>> &triggers,
                         const std::vector<DecoderInstance> &decoders);

}  // namespace son
