#pragma once
#include <libsigrok/libsigrok.h>
#include <libsigrokdecode/libsigrokdecode.h>
#include <glib.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
using json = nlohmann::json;

// A device discovered by a scan, addressable by a stable id from the client.
struct DeviceEntry {
    int id;
    struct sr_dev_driver *drv;
    struct sr_dev_inst *sdi;
};

struct DecoderCfg {
    std::string id;
    std::map<std::string, int> channels; // decoder channel id -> logic bit index
    json options;                        // option id -> value (from JSON)
};

struct CaptureCfg {
    int device_id = -1;
    uint64_t samplerate = 1000000;
    std::vector<int> channels;                       // enabled channel indices
    std::string mode = "triggered";                  // or "continuous"
    uint64_t limit_samples = 1000000;                // triggered only
    int capture_ratio = 0;                           // pre-trigger %, 0 = none
    std::vector<std::pair<int, std::string>> triggers; // (channel index, match)
    std::vector<DecoderCfg> decoders;
};

// Globals (single-client server).
extern struct sr_context *g_ctx;
extern std::vector<DeviceEntry> g_devices;
extern std::atomic<struct sr_session *> g_active_session; // set while a capture runs
extern std::atomic<bool> g_stop_requested;
extern std::atomic<bool> g_capture_running;               // true from capture start to end
extern std::atomic<bool> g_first_data;                    // any datafeed sample seen yet
extern std::atomic<bool> g_watchdog_fired;                // watchdog force-stopped capture

DeviceEntry *find_device(int id);
json scan_devices(bool rescan = false);  // (re)populates g_devices, returns SCAN_RESULT json
json list_decoders_json();    // DECODERS_LIST json

// Runs one capture to completion (blocking); streams results on fd. Defined in capture.cpp.
void run_capture(int fd, const CaptureCfg &cfg);

// Post-hoc decode session (client streams captured logic back for decoding):
// begin -> feed chunks -> end. Annotations are batched to fd as they appear.
void *decode_begin(int fd, uint64_t samplerate, const std::vector<DecoderCfg> &decoders);
void decode_feed(void *state, uint64_t start_sample, const uint8_t *data,
                 uint64_t len, int unitsize);
void decode_end(void *state, int fd);
