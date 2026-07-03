#include "app.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#include "imgui.h"
#include "son_protocol.h"
#include "zstd_util.h"

namespace son {

static std::string fmt_secs(double sec) {
    char b[64];
    double a = std::fabs(sec);
    if (a >= 1.0) std::snprintf(b, sizeof(b), "%.7g s", sec);
    else if (a >= 1e-3) std::snprintf(b, sizeof(b), "%.7g ms", sec * 1e3);
    else if (a >= 1e-6) std::snprintf(b, sizeof(b), "%.7g us", sec * 1e6);
    else std::snprintf(b, sizeof(b), "%.7g ns", sec * 1e9);
    return b;
}

// Tiny recursive-descent evaluator for macro expressions over marker variables.
namespace {
struct ExprParser {
    const char *p;
    const std::map<std::string, double> *vars;
    bool ok = true;
    void ws() { while (*p == ' ' || *p == '\t') ++p; }
    double parse() { ws(); double v = expr(); ws(); if (*p) ok = false; return v; }
    double expr() {
        double v = term();
        for (;;) { ws(); if (*p == '+') { ++p; v += term(); }
                   else if (*p == '-') { ++p; v -= term(); } else break; }
        return v;
    }
    double term() {
        double v = factor();
        for (;;) { ws();
            if (*p == '*') { ++p; v *= factor(); }
            else if (*p == '/') { ++p; double d = factor(); v = d != 0 ? v / d : (ok = false, 0); }
            else break; }
        return v;
    }
    double factor() {
        ws();
        if (*p == '(') { ++p; double v = expr(); ws(); if (*p == ')') ++p; else ok = false; return v; }
        if (*p == '-') { ++p; return -factor(); }
        if (*p == '+') { ++p; return factor(); }
        if (std::isalpha((unsigned char)*p) || *p == '_') {
            std::string id;
            while (std::isalnum((unsigned char)*p) || *p == '_') id += *p++;
            ws();
            if (*p == '(') {
                ++p; double a = expr(); ws(); if (*p == ')') ++p; else ok = false;
                if (id == "abs") return std::fabs(a);
                if (id == "log10") return a > 0 ? std::log10(a) : (ok = false, 0.0);
                ok = false; return 0;
            }
            auto it = vars->find(id);
            if (it == vars->end()) { ok = false; return 0; }
            return it->second;
        }
        char *e; double d = std::strtod(p, &e);
        if (e == p) { ok = false; return 0; }
        p = e; return d;
    }
};
}  // namespace

double App::eval_macro(const std::string &expr, bool &ok) const {
    std::map<std::string, double> vars;
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    for (size_t i = 0; i < markers_.size(); ++i) {
        vars["m" + std::to_string(i + 1)] = markers_[i].sample / sr;  // seconds
        vars["s" + std::to_string(i + 1)] = markers_[i].sample;       // sample index
    }
    vars["sr"] = sr;
    ExprParser ep;
    ep.p = expr.c_str();
    ep.vars = &vars;
    double v = ep.parse();
    ok = ep.ok;
    return v;
}

// ---- small helpers ------------------------------------------------------
static std::string fmt_hz(uint64_t hz) {
    char b[64];
    if (hz >= 1000000000ULL) std::snprintf(b, sizeof(b), "%.3g GHz", hz / 1e9);
    else if (hz >= 1000000ULL) std::snprintf(b, sizeof(b), "%.4g MHz", hz / 1e6);
    else if (hz >= 1000ULL) std::snprintf(b, sizeof(b), "%.4g kHz", hz / 1e3);
    else std::snprintf(b, sizeof(b), "%llu Hz", (unsigned long long)hz);
    return b;
}

static const char *kTrig[] = {"none", "0", "1", "rising", "falling", "edge"};

App::App() { load_settings(); }
App::~App() {
    save_settings();
    do_disconnect();
    for (auto &kv : analog_map_) delete kv.second;
}

std::string App::window_title() const {
    std::string t = "sonview";
    if (connected_)
        t += std::string(" — ") + host_ + (capturing_ ? "  [capturing]" : "");
    else if (conn_state_.load() == 1)
        t += " — connecting…";
    else
        t += " — disconnected";
    return t;
}

void App::set_default_host(const std::string &host) {
    std::snprintf(host_, sizeof(host_), "%s", host.c_str());
}

void App::log(const std::string &s) {
    std::lock_guard<std::mutex> lk(m_);
    log_.push_back(s);
    if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + (log_.size() - 500));
}

SessionMeta App::meta_copy() {
    std::lock_guard<std::mutex> lk(m_);
    return meta_;
}

std::string App::chan_name(int index, const std::string &def) const {
    auto it = chan_names_.find(index);
    return (it != chan_names_.end() && !it->second.empty()) ? it->second : def;
}

// ---- settings persistence -------------------------------------------------
// Everything a bench engineer sets up (host, device, rates, channels, triggers,
// decoders, renames) survives an app restart via ~/.config/sonview/config.json.
std::string App::settings_path() const {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && *xdg ? xdg
                                   : std::string(getenv("HOME") ? getenv("HOME") : ".") +
                                         "/.config";
    return base + "/sonview/config.json";
}

std::string App::dev_key(const DeviceInfo &d) {
    return d.driver + "|" + d.vendor + "|" + d.model + "|" + d.conn;
}

// Stash the selected device's GUI state (channels/triggers/renames/rate).
void App::harvest_device_prefs() {
    if (sel_dev_ < 0 || sel_dev_ >= (int)devices_.size()) return;
    DeviceInfo &d = devices_[sel_dev_];
    json p;
    json en = json::array(), trg = json::object();
    for (auto &ch : d.channels) {
        if (ch.enabled) en.push_back(ch.index);
        if (ch.trigger != "none" && !ch.trigger.empty())
            trg[std::to_string(ch.index)] = ch.trigger;
    }
    p["enabled"] = en;
    p["triggers"] = trg;
    json rn = json::object();
    for (auto &kv : chan_names_) rn[std::to_string(kv.first)] = kv.second;
    p["renames"] = rn;
    p["samplerate_idx"] = samplerate_idx_;
    p["manual_rate_on"] = manual_rate_on_;
    p["manual_rate_hz"] = manual_rate_hz_;
    p["order"] = chan_order_;
    device_prefs_[dev_key(d)] = p;
}

void App::apply_device_prefs(DeviceInfo &d) {
    std::string key = dev_key(d);
    if (!device_prefs_.contains(key)) {
        // First sighting of this device: sensible defaults — all logic channels
        // on (nobody wants to tick 8 boxes before the first capture).
        for (auto &ch : d.channels)
            if (ch.type == "logic") ch.enabled = true;
        return;
    }
    const json &p = device_prefs_[key];
    std::set<int> en;
    if (p.contains("enabled"))
        for (auto &e : p["enabled"]) en.insert(e.get<int>());
    for (auto &ch : d.channels) {
        ch.enabled = en.count(ch.index) > 0;
        ch.trigger = "none";
        if (p.contains("triggers")) {
            auto it = p["triggers"].find(std::to_string(ch.index));
            if (it != p["triggers"].end()) ch.trigger = it->get<std::string>();
        }
    }
}

void App::save_settings() {
    harvest_device_prefs();
    json j;
    j["host"] = host_;
    j["port"] = port_;
    j["auto_reconnect"] = auto_reconnect_;
    j["mode_idx"] = mode_idx_;
    j["limit_ksamples"] = limit_ksamples_;
    j["max_window_msamples"] = max_window_msamples_;
    j["capture_ratio"] = capture_ratio_;
    j["repeat_capture"] = repeat_capture_;
    j["save_path"] = save_path_;
    j["csv_path"] = csv_path_;
    j["sel_dev_key"] = sel_dev_key_;
    j["device_prefs"] = device_prefs_;
    json mj = json::array();
    for (auto &m : macros_) mj.push_back({{"name", m.name}, {"expr", m.expr}});
    j["macros"] = mj;
    json rn = json::object();
    for (auto &kv : chan_names_) rn[std::to_string(kv.first)] = kv.second;
    j["renames"] = rn;

    std::string path = settings_path();
    std::string dir = path.substr(0, path.rfind('/'));
    std::string parent = dir.substr(0, dir.rfind('/'));
    ::mkdir(parent.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::string s = j.dump(1);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

void App::load_settings() {
    FILE *f = std::fopen(settings_path().c_str(), "rb");
    if (!f) return;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    try {
        json j = json::parse(s);
        std::snprintf(host_, sizeof(host_), "%s", j.value("host", std::string(host_)).c_str());
        port_ = j.value("port", port_);
        auto_reconnect_ = j.value("auto_reconnect", auto_reconnect_);
        mode_idx_ = j.value("mode_idx", mode_idx_);
        limit_ksamples_ = j.value("limit_ksamples", limit_ksamples_);
        max_window_msamples_ = j.value("max_window_msamples", max_window_msamples_);
        capture_ratio_ = j.value("capture_ratio", capture_ratio_);
        repeat_capture_ = j.value("repeat_capture", repeat_capture_);
        std::snprintf(save_path_, sizeof(save_path_), "%s",
                      j.value("save_path", std::string(save_path_)).c_str());
        std::snprintf(csv_path_, sizeof(csv_path_), "%s",
                      j.value("csv_path", std::string(csv_path_)).c_str());
        sel_dev_key_ = j.value("sel_dev_key", std::string());
        if (j.contains("device_prefs") && j["device_prefs"].is_object())
            device_prefs_ = j["device_prefs"];
        if (j.contains("macros"))
            for (auto &m : j["macros"])
                macros_.push_back({m.value("name", std::string()), m.value("expr", std::string())});
        if (j.contains("renames"))
            for (auto it = j["renames"].begin(); it != j["renames"].end(); ++it)
                chan_names_[std::stoi(it.key())] = it.value().get<std::string>();
    } catch (const std::exception &e) {
        log(std::string("settings parse error: ") + e.what());
    }
}

// First data of a new capture arrived: NOW replace the previous capture.
// Runs on the RX thread, before the data frame is appended.
void App::commit_pending_capture() {
    SessionMeta m;
    {
        std::lock_guard<std::mutex> lk(m_);
        m = pending_meta_;
    }
    logic_.reset(m.unitsize ? m.unitsize : 1);
    std::vector<int> bits;
    for (auto &ch : m.channels)
        if (ch.type == "logic" && ch.bit >= 0) bits.push_back(ch.bit);
    logic_.set_tracked_bits(bits);
    logic_.set_max_samples(pending_window_);
    anns_.reset();
    {
        // rebuild the display order: keep existing relative order, append new
        std::vector<int> order;
        for (int idx : chan_order_)
            for (auto &ch : m.channels)
                if (ch.index == idx) { order.push_back(idx); break; }
        for (auto &ch : m.channels) {
            bool have = false;
            for (int idx : order)
                if (idx == ch.index) { have = true; break; }
            if (!have) order.push_back(ch.index);
        }
        chan_order_ = std::move(order);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto &kv : analog_map_) delete kv.second;
        analog_map_.clear();
        meta_ = m;
        meta_valid_ = true;
    }
    {
        // The frames buffered since SESSION_META become the new recording.
        std::lock_guard<std::mutex> lk(rec_m_);
        record_ = std::move(pending_record_);
        pending_record_.clear();
        pending_rec_active_ = false;
        record_truncated_ = false;
    }
    meta_pending_ = false;
    user_view_touched_ = false;
    need_view_reset_ = true;
}

AnalogStore *App::analog_get(uint32_t id, bool create) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = analog_map_.find(id);
    if (it != analog_map_.end()) return it->second;
    if (!create) return nullptr;
    AnalogStore *a = new AnalogStore();
    a->reset();
    analog_map_[id] = a;
    return a;
}

// ---- connection / RX ----------------------------------------------------
// Connect runs on the worker thread (which then becomes the RX loop), so a
// wrong IP / powered-off Pi never freezes the UI. The joinable-thread reap at
// the top also fixes the reconnect-after-server-death crash: rx_loop used to
// self-exit leaving rx_thread_ joinable, and the next connect's move-assign
// called std::terminate.
void App::do_connect() {
    if (connected_ || conn_state_.load() == 1) return;
    rx_stop_ = true;
    client_.close();
    if (rx_thread_.joinable()) rx_thread_.join();  // reap a dead RX thread
    rx_stop_ = false;
    conn_state_ = 1;  // connecting…
    std::string host = host_;
    uint16_t port = (uint16_t)port_;
    rx_thread_ = std::thread([this, host, port] {
        std::string err;
        if (!client_.connect(host, port, err)) {
            {
                std::lock_guard<std::mutex> lk(m_);
                conn_err_ = err;
            }
            conn_state_ = 2;
            log("connect failed: " + err);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            server_info_.clear();
            conn_err_.clear();
            ctrl_.clear();
        }
        connected_ = true;
        conn_lost_ = false;
        conn_state_ = 0;
        client_.send_json(SON_MSG_HELLO, R"({"client":"sonview","proto":1})");
        client_.send_empty(SON_MSG_SCAN_REQ);
        client_.send_empty(SON_MSG_DECODERS_REQ);
        log(std::string("connected to ") + host);
        rx_loop();
    });
}

void App::do_disconnect() {
    rx_stop_ = true;
    client_.close();
    if (rx_thread_.joinable()) rx_thread_.join();
    connected_ = false;
    capturing_ = false;
    conn_state_ = 0;
    conn_lost_ = false;  // explicit disconnect: no auto-reconnect
}

void App::rx_loop() {
    son_msg msg;
    while (!rx_stop_) {
        if (!client_.recv(msg)) break;
        record_frame(msg);
        process_rx_message(msg);
    }
    connected_ = false;
    capturing_ = false;
    if (!rx_stop_) {  // unexpected loss (server died / network partition)
        conn_lost_ = true;
        log("connection lost");
    }
}

void App::process_rx_message(son_msg &msg) {
    if (!maybe_decompress(msg)) { log("ERROR: zstd decompress failed"); return; }
    {
        uint8_t t = msg.hdr.type;

        if (t == SON_MSG_SESSION_META) {
            SessionMeta m;
            std::string err;
            if (parse_session_meta(msg.json(), m, err)) {
                // Deferred start: stash the metadata but wipe NOTHING yet. The
                // previous capture survives until the new one actually delivers
                // data (commit_pending_capture), so an armed capture that never
                // triggers cannot destroy captured evidence.
                {
                    std::lock_guard<std::mutex> lk(m_);
                    pending_meta_ = m;
                }
                meta_pending_ = true;
                has_trigger_ = false;
                capturing_ = true;
            } else {
                log("SESSION_META parse error: " + err);
            }
            return;
        }
        if (t == SON_MSG_TRIGGER) {
            try {
                json j = json::parse(msg.json());
                trigger_sample_ = j.value("sample", (uint64_t)0);
                has_trigger_ = true;
            } catch (...) {}
            return;
        }
        if (t == SON_MSG_DATA_LOGIC) {
            if (msg.hdr.flags & SON_FLAG_ZSTD) { log("ERROR: ZSTD logic payload unsupported"); return; }
            if (meta_pending_.load()) commit_pending_capture();
            if (msg.payload.size() >= sizeof(son_logic_hdr)) {
                son_logic_hdr h;
                std::memcpy(&h, msg.payload.data(), sizeof(h));
                size_t need = (size_t)h.sample_count * h.unitsize;
                if (msg.payload.size() >= sizeof(son_logic_hdr) + need) {
                    bool disc = (msg.hdr.flags & SON_FLAG_DISCONTINUITY) != 0;
                    logic_.append(h.start_sample, h.sample_count,
                                  msg.payload.data() + sizeof(son_logic_hdr), disc);
                    if (pending_window_ == 0 && capturing_ && logic_.full()) {
                        // Unbounded continuous capture reached the 2^32-sample
                        // store capacity: stop honestly instead of dropping.
                        log("sample store full (2^32) — stopping capture");
                        client_.send_empty(SON_MSG_STOP);
                    }
                    if (pending_window_)
                        anns_.trim_before(logic_.first_live());
                }
            }
            return;
        }
        if (t == SON_MSG_DATA_ANALOG) {
            if (msg.hdr.flags & SON_FLAG_ZSTD) { log("ERROR: ZSTD analog payload unsupported"); return; }
            if (meta_pending_.load()) commit_pending_capture();
            if (msg.payload.size() >= sizeof(son_analog_hdr)) {
                son_analog_hdr h;
                std::memcpy(&h, msg.payload.data(), sizeof(h));
                size_t need = (size_t)h.sample_count * sizeof(float);
                if (msg.payload.size() >= sizeof(son_analog_hdr) + need) {
                    AnalogStore *as = analog_get(h.channel_id, true);
                    as->append(h.start_sample, h.sample_count,
                               (const float *)(msg.payload.data() + sizeof(son_analog_hdr)));
                }
            }
            return;
        }
        if (t == SON_MSG_ANN_BATCH) {
            if (msg.hdr.flags & SON_FLAG_ZSTD) { log("ERROR: ZSTD annotation payload unsupported"); return; }
            if (msg.payload.size() >= sizeof(son_ann_batch_hdr)) {
                son_ann_batch_hdr bh;
                std::memcpy(&bh, msg.payload.data(), sizeof(bh));
                size_t off = sizeof(son_ann_batch_hdr);
                for (uint16_t i = 0; i < bh.count; ++i) {
                    if (off + sizeof(son_ann) > msg.payload.size()) break;
                    son_ann a;
                    std::memcpy(&a, msg.payload.data() + off, sizeof(a));
                    off += sizeof(son_ann);
                    Annotation an;
                    an.start = a.start_sample;
                    an.end = a.end_sample;
                    an.ann_class = a.ann_class;
                    bool ok = true;
                    for (uint16_t k = 0; k < a.n_texts; ++k) {
                        if (off + 2 > msg.payload.size()) { ok = false; break; }
                        uint16_t len;
                        std::memcpy(&len, msg.payload.data() + off, 2);
                        off += 2;
                        if (off + len > msg.payload.size()) { ok = false; break; }
                        an.texts.emplace_back((const char *)msg.payload.data() + off, len);
                        off += len;
                    }
                    anns_.add(bh.decode_stack_id, bh.row_id, an);
                    if (!ok) break;
                }
            }
            return;
        }
        if (t == SON_MSG_CLIENT_STATE) { apply_client_state(msg.json()); return; }
        if (t == SON_MSG_CAPTURE_END) {
            capturing_ = false;
            if (meta_pending_.exchange(false)) {
                // Armed capture ended with zero data (trigger never fired /
                // timeout / error): the previous capture is untouched.
                log("capture ended with no data — previous capture kept");
            } else if (!user_view_touched_.load()) {
                // Fit the completed capture only if the user hasn't already
                // panned/zoomed to a point of interest.
                need_view_reset_ = true;
            }
        }

        // Everything else goes to the main thread.
        {
            std::lock_guard<std::mutex> lk(m_);
            ctrl_.push_back(msg);
        }
    }
}

void App::process_ctrl() {
    std::vector<son_msg> local;
    {
        std::lock_guard<std::mutex> lk(m_);
        local.swap(ctrl_);
    }
    for (auto &msg : local) {
        std::string err;
        switch (msg.hdr.type) {
            case SON_MSG_SERVER_INFO:
                server_info_ = msg.json();
                log("SERVER_INFO received");
                break;
            case SON_MSG_SCAN_RESULT: {
                std::vector<DeviceInfo> devs;
                if (parse_scan_result(msg.json(), devs, err)) {
                    // Merge: never lose channel enables/triggers on a rescan or
                    // reconnect. Prefer live GUI state, else persisted prefs.
                    std::string want = (sel_dev_ >= 0 && sel_dev_ < (int)devices_.size())
                                           ? dev_key(devices_[sel_dev_])
                                           : sel_dev_key_;
                    for (auto &nd : devs) {
                        const DeviceInfo *old = nullptr;
                        for (auto &od : devices_)
                            if (dev_key(od) == dev_key(nd)) { old = &od; break; }
                        if (old) {
                            for (auto &nc : nd.channels)
                                for (auto &oc : old->channels)
                                    if (oc.index == nc.index) {
                                        nc.enabled = oc.enabled;
                                        nc.trigger = oc.trigger;
                                    }
                        } else {
                            apply_device_prefs(nd);
                        }
                    }
                    devices_ = std::move(devs);
                    sel_dev_ = -1;
                    for (int i = 0; i < (int)devices_.size(); ++i)
                        if (dev_key(devices_[i]) == want) { sel_dev_ = i; break; }
                    if (sel_dev_ < 0 && !devices_.empty()) sel_dev_ = 0;
                    if (sel_dev_ >= 0) {
                        sel_dev_key_ = dev_key(devices_[sel_dev_]);
                        // restore this device's persisted samplerate choice
                        if (device_prefs_.contains(sel_dev_key_)) {
                            const json &p = device_prefs_[sel_dev_key_];
                            samplerate_idx_ = p.value("samplerate_idx", samplerate_idx_);
                            manual_rate_on_ = p.value("manual_rate_on", manual_rate_on_);
                            manual_rate_hz_ = p.value("manual_rate_hz", manual_rate_hz_);
                            if (chan_order_.empty() && p.contains("order"))
                                for (auto &o : p["order"]) chan_order_.push_back(o.get<int>());
                        }
                    }
                    log("SCAN_RESULT: " + std::to_string(devices_.size()) + " device(s)");
                } else log("SCAN_RESULT parse error: " + err);
                break;
            }
            case SON_MSG_DECODERS_LIST: {
                std::vector<DecoderMeta> cat;
                if (parse_decoders_list(msg.json(), cat, err)) {
                    catalog_ = std::move(cat);
                    log("DECODERS_LIST: " + std::to_string(catalog_.size()) + " decoder(s)");
                } else log("DECODERS_LIST parse error: " + err);
                break;
            }
            case SON_MSG_DECODER_INFO: {
                std::vector<DecoderRowInfo> ri;
                if (parse_decoder_info(msg.json(), ri, err)) decoder_rows_ = std::move(ri);
                break;
            }
            case SON_MSG_CAPTURE_END: {
                std::string reason;
                try {
                    json j = json::parse(msg.json());
                    reason = j.value("reason", "");
                    capture_reason_ = reason;
                    std::string mm = j.value("message", "");
                    if (!mm.empty()) capture_reason_ += ": " + mm;
                } catch (...) { capture_reason_ = msg.json(); }
                log("CAPTURE_END: " + capture_reason_);
                // Auto re-arm: catch intermittent events without babysitting.
                if (repeat_capture_ && reason == "complete" && connected_ &&
                    !capturing_ && mode_idx_ == 0)
                    start_capture();
                break;
            }
            case SON_MSG_DECODE_END:
                redecoding_ = false;
                log("re-decode complete: " + std::to_string(anns_.total()) +
                    " annotation(s)");
                break;
            case SON_MSG_ERROR:
                redecoding_ = false;
                log("SERVER ERROR: " + msg.json());
                break;
            default:
                break;
        }
    }
}

// ---- capture recording / save / load ------------------------------------
// A .son file is just the recorded protocol frames (SESSION_META + DATA_* +
// DECODER_INFO + ANN_BATCH + CAPTURE_END), so replay reuses process_rx_message
// and works fully offline.
void App::record_frame(const son_msg &msg) {
    std::lock_guard<std::mutex> lk(rec_m_);
    uint8_t t = msg.hdr.type;
    if (t == SON_MSG_CLIENT_STATE) return;  // appended fresh at save time
    // Frames from SESSION_META up to the first data go into a side buffer; the
    // committed recording (= the previous capture) is only replaced when the
    // new capture actually produces data (see commit_pending_capture).
    if (t == SON_MSG_SESSION_META) {
        pending_record_.clear();
        pending_rec_active_ = true;
    }
    std::vector<uint8_t> *buf = pending_rec_active_ ? &pending_record_ : &record_;
    if (buf == &record_ && record_.empty()) return;  // pre-capture control traffic
    const size_t CAP = 512u * 1024 * 1024;
    size_t frame = sizeof(son_hdr) + msg.payload.size();
    if (buf->size() + frame > CAP) { record_truncated_ = true; return; }
    const uint8_t *h = reinterpret_cast<const uint8_t *>(&msg.hdr);
    buf->insert(buf->end(), h, h + sizeof(son_hdr));
    buf->insert(buf->end(), msg.payload.begin(), msg.payload.end());
}

std::string App::client_state_dump() const {
    json j;
    j["markers"] = json::array();
    for (auto &m : markers_) j["markers"].push_back({{"name", m.name}, {"sample", m.sample}});
    j["macros"] = json::array();
    for (auto &mc : macros_) j["macros"].push_back({{"name", mc.name}, {"expr", mc.expr}});
    j["channel_names"] = json::object();
    for (auto &kv : chan_names_) j["channel_names"][std::to_string(kv.first)] = kv.second;
    return j.dump();
}

void App::apply_client_state(const std::string &js) {
    try {
        json j = json::parse(js);
        markers_.clear();
        if (j.contains("markers"))
            for (auto &m : j["markers"])
                markers_.push_back({m.value("sample", 0.0), m.value("name", std::string("m"))});
        marker_seq_ = (int)markers_.size();
        macros_.clear();
        if (j.contains("macros"))
            for (auto &mc : j["macros"])
                macros_.push_back({mc.value("name", std::string()), mc.value("expr", std::string())});
        chan_names_.clear();
        if (j.contains("channel_names"))
            for (auto it = j["channel_names"].begin(); it != j["channel_names"].end(); ++it)
                chan_names_[std::stoi(it.key())] = it.value().get<std::string>();
    } catch (const std::exception &e) {
        log(std::string("client-state parse error: ") + e.what());
    }
}

bool App::save_capture(const std::string &path, std::string &err) {
    std::lock_guard<std::mutex> lk(rec_m_);
    if (record_.empty()) { err = "nothing captured yet"; return false; }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open " + path; return false; }
    const unsigned char magic[8] = {'S', 'O', 'N', 'C', 'A', 'P', '\0', 1};
    std::fwrite(magic, 1, 8, f);
    std::fwrite(record_.data(), 1, record_.size(), f);
    // Append current markers/macros/renames as a trailing CLIENT_STATE frame.
    std::string st = client_state_dump();
    son_hdr h;
    h.magic = SON_MAGIC; h.version = SON_PROTO_VERSION; h.type = SON_MSG_CLIENT_STATE;
    h.flags = 0; h.stream_id = 0; h.length = (uint32_t)st.size();
    std::fwrite(&h, 1, sizeof(h), f);
    std::fwrite(st.data(), 1, st.size(), f);
    std::fclose(f);
    return true;
}

bool App::load_capture(const std::string &path, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    unsigned char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "SONCAP", 6) != 0) {
        std::fclose(f); err = "not a SONCAP file"; return false;
    }
    do_disconnect();  // own the stores; stop any live RX
    std::vector<uint8_t> rec;
    son_msg msg;
    for (;;) {
        son_hdr h;
        if (std::fread(&h, 1, sizeof(h), f) != sizeof(h)) break;  // EOF
        if (h.magic != SON_MAGIC) { err = "corrupt frame"; break; }
        msg.hdr = h;
        msg.payload.resize(h.length);
        if (h.length && std::fread(msg.payload.data(), 1, h.length, f) != h.length) {
            err = "truncated file"; break;
        }
        if (h.type != SON_MSG_CLIENT_STATE) {  // re-appended fresh on save
            const uint8_t *hp = reinterpret_cast<const uint8_t *>(&h);
            rec.insert(rec.end(), hp, hp + sizeof(h));
            rec.insert(rec.end(), msg.payload.begin(), msg.payload.end());
        }
        process_rx_message(msg);
    }
    std::fclose(f);
    { std::lock_guard<std::mutex> lk(rec_m_); record_.swap(rec); }
    process_ctrl();  // apply DECODER_INFO / CAPTURE_END queued during replay
    return err.empty();
}

int App::replay_and_report(const std::string &path, const std::string &export_base) {
    std::string err;
    if (!load_capture(path, err)) { std::fprintf(stderr, "replay failed: %s\n", err.c_str()); return 1; }
    if (!export_base.empty()) {
        std::string e;
        if (export_vcd(export_base + ".vcd", e))
            std::printf("exported %s.vcd\n", export_base.c_str());
        else
            std::printf("VCD export failed: %s\n", e.c_str());
        if (export_sr(export_base + ".sr", e))
            std::printf("exported %s.sr\n", export_base.c_str());
        else
            std::printf(".sr export failed: %s\n", e.c_str());
    }
    SessionMeta m = meta_copy();
    std::printf("replay %s: samplerate=%llu unitsize=%u channels=%zu logic_samples=%llu "
                "annotations=%zu markers=%zu macros=%zu renames=%zu\n",
                path.c_str(), (unsigned long long)m.samplerate, (unsigned)m.unitsize,
                m.channels.size(), (unsigned long long)logic_.count(), anns_.total(),
                markers_.size(), macros_.size(), chan_names_.size());
    for (auto &mk : markers_)
        std::printf("  marker %s @ %.0f\n", mk.name.c_str(), mk.sample);
    for (auto &kv : chan_names_)
        std::printf("  rename ch%d -> %s\n", kv.first, kv.second.c_str());
    return 0;
}

int run_replay(const std::string &path, const std::string &export_base) {
    App app;
    return app.replay_and_report(path, export_base);
}

// ---- post-hoc re-decode ---------------------------------------------------
// Re-run the configured decoder stack over the CAPTURED data (or a loaded
// .son) by streaming it back to the server's decode session.
void App::start_redecode() {
    start_error_.clear();
    if (!connected_ || capturing_ || redecoding_.load()) return;
    uint64_t fl = logic_.first_live(), total = logic_.count();
    if (total <= fl) { start_error_ = "no captured data to decode"; return; }
    if (decoders_.empty()) { start_error_ = "no decoders configured"; return; }
    for (auto &dec : decoders_) {
        for (auto &c : dec.meta.channels) {
            if (!c.required) continue;
            auto it = dec.ch_map.find(c.id);
            if (it == dec.ch_map.end() || it->second < 0) {
                start_error_ = dec.meta.name + ": required pin '" + c.name + "' not mapped";
                return;
            }
        }
    }
    SessionMeta m = meta_copy();
    int unitsize = logic_.unitsize() ? logic_.unitsize() : 1;
    anns_.reset();
    redecoding_ = true;

    json req;
    req["samplerate"] = m.samplerate;
    req["unitsize"] = unitsize;
    req["decoders"] = decoders_to_json(decoders_);
    if (!client_.send_json(SON_MSG_DECODE_REQ, req.dump())) {
        redecoding_ = false;
        return;
    }
    // Stream the store back in chunks (uncompressed; LAN is not the bottleneck).
    const uint32_t CHUNK = 65536;
    std::vector<uint8_t> buf;
    for (uint64_t s = fl; s < total; s += CHUNK) {
        uint32_t n = (uint32_t)std::min<uint64_t>(CHUNK, total - s);
        buf.resize(sizeof(son_logic_hdr) + (size_t)n * unitsize);
        son_logic_hdr h;
        h.start_sample = s;
        h.sample_count = n;
        h.samples_dropped = 0;
        h.unitsize = (uint8_t)unitsize;
        std::memset(h._pad, 0, sizeof(h._pad));
        std::memcpy(buf.data(), &h, sizeof(h));
        logic_.copy_raw(s, n, buf.data() + sizeof(h));
        if (!son_send(client_.fd(), SON_MSG_DATA_LOGIC, 0, 1, buf.data(),
                      (uint32_t)buf.size())) {
            redecoding_ = false;
            log("re-decode: send failed");
            return;
        }
    }
    client_.send_empty(SON_MSG_DECODE_END);
    log("re-decode: " + std::to_string(total - fl) + " samples sent, decoding...");
}

// ---- between-marker measurements -------------------------------------------
void App::update_measurements() {
    if (markers_.size() < 2) { meas_ = MeasCache{}; return; }
    double a = markers_[markers_.size() - 2].sample;
    double b = markers_[markers_.size() - 1].sample;
    if (a > b) std::swap(a, b);
    uint64_t cnt = logic_.count();
    if (a == meas_.a && b == meas_.b && cnt == meas_.count) return;  // cached
    meas_.a = a; meas_.b = b; meas_.count = cnt;
    meas_.lines.clear();

    SessionMeta m = meta_copy();
    double sr = m.samplerate > 0 ? (double)m.samplerate : 1.0;
    uint64_t lo = (uint64_t)std::max(a, (double)logic_.first_live());
    uint64_t hi = (uint64_t)std::min(b, (double)cnt);
    if (hi <= lo) return;
    double dt = (double)(hi - lo) / sr;
    if (hi - lo > 50000000ULL) {  // keep the per-frame cost bounded
        meas_.lines.push_back("(interval > 50M samples — too large to analyse)");
        return;
    }
    char l[160];
    for (auto &ch : m.channels) {
        std::string nm = chan_name(ch.index, ch.name);
        if (ch.type == "logic" && ch.bit >= 0) {
            std::vector<Edge> e;
            uint8_t init;
            logic_.walk(ch.bit, lo, hi, 1.0, init, e, nullptr);
            // integrate high time for duty cycle
            uint64_t high = 0, prev = lo;
            uint8_t cur = init;
            for (auto &ed : e) {
                if (cur) high += ed.sample - prev;
                prev = ed.sample;
                cur = ed.value;
            }
            if (cur) high += hi - prev;
            double duty = 100.0 * (double)high / (double)(hi - lo);
            double freq = e.size() >= 2 ? (double)e.size() / 2.0 / dt : 0.0;
            std::snprintf(l, sizeof(l), "%-8s %zu edges  f=%.5g Hz  duty=%.1f%%",
                          nm.c_str(), e.size(), freq, duty);
            meas_.lines.push_back(l);
        } else if (ch.type == "analog") {
            AnalogStore *as = analog_get((uint32_t)ch.index, false);
            if (!as || as->count() <= lo) continue;
            uint64_t ahi = std::min<uint64_t>(hi, as->count());
            uint64_t stride = std::max<uint64_t>(1, (ahi - lo) / 200000);
            double mn = 1e30, mx = -1e30, sum = 0;
            uint64_t n = 0;
            for (uint64_t s = lo; s < ahi; s += stride) {
                double v = as->value(s);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
                sum += v;
                ++n;
            }
            if (!n) continue;
            std::snprintf(l, sizeof(l), "%-8s min=%.4g max=%.4g pp=%.4g mean=%.4g%s",
                          nm.c_str(), mn, mx, mx - mn, sum / (double)n,
                          stride > 1 ? " (sampled)" : "");
            meas_.lines.push_back(l);
        }
    }
}

// Channels in user display order (chan_order_), falling back to meta order.
std::vector<const ChannelInfo *> App::ordered_channels(const SessionMeta &m) const {
    std::vector<const ChannelInfo *> out;
    for (int idx : chan_order_)
        for (auto &ch : m.channels)
            if (ch.index == idx) { out.push_back(&ch); break; }
    for (auto &ch : m.channels) {
        bool have = false;
        for (auto *p : out)
            if (p->index == ch.index) { have = true; break; }
        if (!have) out.push_back(&ch);
    }
    return out;
}

// ---- capture ------------------------------------------------------------
static uint64_t effective_rate(const DeviceInfo &d, int idx, bool manual, int manual_hz) {
    if (manual || d.samplerates.empty()) return (uint64_t)manual_hz;
    if (idx < 0 || idx >= (int)d.samplerates.size()) idx = 0;
    return d.samplerates[idx];
}

void App::start_capture() {
    start_error_.clear();
    if (sel_dev_ < 0 || sel_dev_ >= (int)devices_.size()) return;
    DeviceInfo &d = devices_[sel_dev_];
    std::vector<int> channels;
    std::vector<std::pair<int, std::string>> triggers;
    for (auto &ch : d.channels) {
        if (!ch.enabled) continue;
        channels.push_back(ch.index);
        if (ch.type == "logic" && ch.trigger != "none")
            triggers.push_back({ch.index, ch.trigger});
    }
    if (channels.empty()) {
        start_error_ = "no channels enabled";
        log("start: " + start_error_);
        return;
    }
    // Validate decoder wiring up front with a visible message, instead of a
    // silently empty decode lane.
    for (auto &dec : decoders_) {
        for (auto &c : dec.meta.channels) {
            if (!c.required) continue;
            auto it = dec.ch_map.find(c.id);
            if (it == dec.ch_map.end() || it->second < 0) {
                start_error_ = dec.meta.name + ": required pin '" + c.name + "' not mapped";
                log("start: " + start_error_);
                return;
            }
        }
    }

    uint64_t rate = effective_rate(d, samplerate_idx_, manual_rate_on_, manual_rate_hz_);
    std::string mode = mode_idx_ == 0 ? "triggered" : "continuous";
    uint64_t limit = (uint64_t)limit_ksamples_ * 1000ULL;
    // The rolling window is applied when the capture commits (first data), so
    // arming can never trim the previous capture that is still on screen.
    pending_window_ = mode == "continuous" ? (uint64_t)max_window_msamples_ * 1000000ULL : 0;
    last_cfg_triggered_ = !triggers.empty();

    int ratio = (mode == "triggered" && !triggers.empty()) ? capture_ratio_ : 0;
    std::string cfg = build_config(d.id, rate, channels, mode, limit, ratio, triggers, decoders_);
    if (!client_.send_json(SON_MSG_CONFIG, cfg)) { log("send CONFIG failed"); return; }
    if (!client_.send_empty(SON_MSG_START)) { log("send START failed"); return; }
    capture_reason_.clear();
    log("capture started (" + mode + ", " + fmt_hz(rate) + ")");
    save_settings();  // the config that just worked is worth keeping
}

void App::stop_capture() {
    client_.send_empty(SON_MSG_STOP);
    log("stop requested");
}

// ---- panels -------------------------------------------------------------
void App::draw_connection_panel() {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 118), ImGuiCond_FirstUseEver);
    ImGui::Begin("Connection");
    ImGui::InputText("Host", host_, sizeof(host_));
    ImGui::InputInt("Port", &port_);
    int cs = conn_state_.load();
    if (connected_) {
        if (ImGui::Button("Disconnect")) do_disconnect();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "connected");
    } else if (cs == 1) {
        ImGui::BeginDisabled();
        ImGui::Button("Connect");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "connecting...");
    } else {
        if (ImGui::Button("Connect")) do_connect();
        ImGui::SameLine();
        if (conn_lost_.load())
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
                               auto_reconnect_ ? "connection lost - retrying" : "connection lost");
        else if (cs == 2) {
            std::string e;
            {
                std::lock_guard<std::mutex> lk(m_);
                e = conn_err_;
            }
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "failed: %s", e.c_str());
        } else
            ImGui::TextDisabled("disconnected");
    }
    ImGui::Checkbox("Auto-reconnect", &auto_reconnect_);
    if (!server_info_.empty()) ImGui::TextWrapped("%s", server_info_.c_str());
    ImGui::End();
}

void App::draw_device_panel() {
    ImGui::SetNextWindowPos(ImVec2(8, 130), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 360), ImGuiCond_FirstUseEver);
    ImGui::Begin("Device");
    if (devices_.empty()) {
        ImGui::TextDisabled("No devices. Connect to scan.");
        ImGui::End();
        return;
    }
    std::string cur = (sel_dev_ >= 0) ? devices_[sel_dev_].label() : "(select)";
    ImGui::SetNextItemWidth(-72);
    if (ImGui::BeginCombo("##device", cur.c_str())) {
        for (int i = 0; i < (int)devices_.size(); ++i)
            if (ImGui::Selectable(devices_[i].label().c_str(), i == sel_dev_)) {
                if (i != sel_dev_) {
                    harvest_device_prefs();  // stash the old device's setup
                    sel_dev_ = i;
                    sel_dev_key_ = dev_key(devices_[i]);
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan") && connected_)
        client_.send_json(SON_MSG_SCAN_REQ, R"({"rescan":true})");
    ImGui::SetItemTooltip("Re-enumerate hardware (picks up hot-plugged devices)");
    if (sel_dev_ < 0) { ImGui::End(); return; }
    DeviceInfo &d = devices_[sel_dev_];

    // samplerate
    if (!d.samplerates.empty()) {
        ImGui::Checkbox("Manual rate", &manual_rate_on_);
        if (!manual_rate_on_) {
            std::string pv = fmt_hz(d.samplerates[
                (samplerate_idx_ >= 0 && samplerate_idx_ < (int)d.samplerates.size())
                    ? samplerate_idx_ : 0]);
            if (ImGui::BeginCombo("Samplerate", pv.c_str())) {
                for (int i = 0; i < (int)d.samplerates.size(); ++i)
                    if (ImGui::Selectable(fmt_hz(d.samplerates[i]).c_str(), i == samplerate_idx_))
                        samplerate_idx_ = i;
                ImGui::EndCombo();
            }
        }
    } else {
        manual_rate_on_ = true;
        ImGui::TextDisabled("device reports no samplerates; enter manually");
    }
    if (manual_rate_on_) ImGui::InputInt("Rate (Hz)", &manual_rate_hz_);

    ImGui::RadioButton("triggered", &mode_idx_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("continuous", &mode_idx_, 1);
    uint64_t rate_now = effective_rate(d, samplerate_idx_, manual_rate_on_, manual_rate_hz_);
    if (mode_idx_ == 0) {
        ImGui::InputInt("Samples (x1000)", &limit_ksamples_);
        if (limit_ksamples_ < 1) limit_ksamples_ = 1;
        if (rate_now > 0) {
            // Show what those samples MEAN at the chosen rate.
            double dur = (double)limit_ksamples_ * 1000.0 / (double)rate_now;
            ImGui::SameLine();
            ImGui::TextDisabled("= %s", fmt_secs(dur).c_str());
        }
        bool any_trig = false;
        for (auto &ch : d.channels)
            if (ch.enabled && ch.trigger != "none" && !ch.trigger.empty()) any_trig = true;
        if (any_trig) {
            ImGui::SliderInt("Pre-trigger %", &capture_ratio_, 0, 99);
            ImGui::SetItemTooltip("Portion of the buffer captured BEFORE the trigger\n"
                                  "(see what caused the event). Best-effort per device.");
        }
    } else {
        ImGui::InputInt("Window (Msamples)", &max_window_msamples_);
        if (max_window_msamples_ < 1) max_window_msamples_ = 1;
        if (rate_now > 0) {
            double dur = (double)max_window_msamples_ * 1e6 / (double)rate_now;
            ImGui::SameLine();
            ImGui::TextDisabled("= %s", fmt_secs(dur).c_str());
        }
    }

    ImGui::SeparatorText("Channels");
    if (ImGui::SmallButton("all")) {
        for (auto &ch : d.channels) ch.enabled = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("none")) {
        for (auto &ch : d.channels) ch.enabled = false;
    }
    ImGui::SameLine();
    if (d.triggers)
        ImGui::TextDisabled("multi-channel triggers AND-combine");
    else
        ImGui::TextDisabled("(this device has no trigger support)");
    for (auto &ch : d.channels) {
        ImGui::PushID(ch.index);
        ImGui::Checkbox("##en", &ch.enabled);
        ImGui::SameLine();
        char nm[24];
        std::snprintf(nm, sizeof(nm), "%s", chan_name(ch.index, ch.name).c_str());
        ImGui::SetNextItemWidth(96);
        if (ImGui::InputText("##nm", nm, sizeof(nm))) chan_names_[ch.index] = nm;
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", ch.type.c_str());
        if (ch.type == "logic" && d.triggers) {
            ImGui::SameLine();
            int ti = 0;
            for (int k = 0; k < 6; ++k) if (ch.trigger == kTrig[k]) ti = k;
            ImGui::SetNextItemWidth(90);
            if (ImGui::BeginCombo("##trig", kTrig[ti])) {
                for (int k = 0; k < 6; ++k)
                    if (ImGui::Selectable(kTrig[k], k == ti)) ch.trigger = kTrig[k];
                ImGui::EndCombo();
            }
        }
        // waveform display order
        ImGui::SameLine();
        bool mv_up = ImGui::SmallButton("^");
        ImGui::SameLine();
        bool mv_dn = ImGui::SmallButton("v");
        ImGui::SetItemTooltip("Move this channel in the waveform view");
        if (mv_up || mv_dn) {
            // ensure every channel is present in the order list first
            for (auto &c2 : d.channels) {
                bool have = false;
                for (int idx : chan_order_)
                    if (idx == c2.index) { have = true; break; }
                if (!have) chan_order_.push_back(c2.index);
            }
            for (size_t k = 0; k < chan_order_.size(); ++k) {
                if (chan_order_[k] != ch.index) continue;
                if (mv_up && k > 0) std::swap(chan_order_[k], chan_order_[k - 1]);
                else if (mv_dn && k + 1 < chan_order_.size())
                    std::swap(chan_order_[k], chan_order_[k + 1]);
                break;
            }
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void App::draw_decoder_panel() {
    ImGui::SetNextWindowPos(ImVec2(8, 494), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 232), ImGuiCond_FirstUseEver);
    ImGui::Begin("Decoders");
    // enabled channels of the selected device (for role mapping)
    std::vector<std::pair<int, std::string>> chans;  // index, name
    if (sel_dev_ >= 0 && sel_dev_ < (int)devices_.size())
        for (auto &c : devices_[sel_dev_].channels)
            if (c.enabled) chans.push_back({c.index, chan_name(c.index, c.name)});

    if (catalog_.empty()) {
        ImGui::TextDisabled("No decoder catalog (connect first).");
    } else {
        if (add_decoder_idx_ >= (int)catalog_.size()) add_decoder_idx_ = 0;
        ImGui::SetNextItemWidth(110);
        ImGui::InputTextWithHint("##dfilter", "filter", dec_filter_, sizeof(dec_filter_));
        ImGui::SameLine();
        auto matches = [&](const DecoderMeta &m) {
            if (!dec_filter_[0]) return true;
            std::string f = dec_filter_, hay = m.id + " " + m.name + " " + m.longname;
            for (auto &c : f) c = (char)tolower((unsigned char)c);
            for (auto &c : hay) c = (char)tolower((unsigned char)c);
            return hay.find(f) != std::string::npos;
        };
        if (!matches(catalog_[add_decoder_idx_]))
            for (int i = 0; i < (int)catalog_.size(); ++i)
                if (matches(catalog_[i])) { add_decoder_idx_ = i; break; }
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##addsel", catalog_[add_decoder_idx_].name.c_str())) {
            for (int i = 0; i < (int)catalog_.size(); ++i) {
                if (!matches(catalog_[i])) continue;
                std::string item = catalog_[i].name;
                if (!catalog_[i].longname.empty() && catalog_[i].longname != catalog_[i].name)
                    item += " — " + catalog_[i].longname;
                if (ImGui::Selectable(item.c_str(), i == add_decoder_idx_))
                    add_decoder_idx_ = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            DecoderInstance inst;
            inst.meta = catalog_[add_decoder_idx_];
            init_decoder_defaults(inst);
            decoders_.push_back(inst);
        }
    }

    // Post-hoc decode: run the configured stack over already-captured data
    // (works on loaded .son files too; needs a connected server).
    bool can_rd = connected_ && !capturing_ && !redecoding_.load() &&
                  logic_.count() > logic_.first_live() && !decoders_.empty();
    if (!can_rd) ImGui::BeginDisabled();
    if (ImGui::Button("Re-decode captured data")) start_redecode();
    if (!can_rd) ImGui::EndDisabled();
    if (redecoding_.load()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "decoding...");
    }

    int remove = -1;
    for (int i = 0; i < (int)decoders_.size(); ++i) {
        DecoderInstance &d = decoders_[i];
        ImGui::PushID(i);
        std::string hdr = d.meta.name + "##dec" + std::to_string(i);
        if (ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::SmallButton("Remove")) remove = i;
            ImGui::TextDisabled("Channels");
            for (auto &c : d.meta.channels) {
                int curidx = d.ch_map.count(c.id) ? d.ch_map[c.id] : -1;
                std::string cur = "(none)";
                for (auto &kv : chans) if (kv.first == curidx) cur = kv.second;
                std::string lbl = c.name + (c.required ? "*" : "");
                if (ImGui::BeginCombo(lbl.c_str(), cur.c_str())) {
                    if (ImGui::Selectable("(none)", curidx < 0)) d.ch_map[c.id] = -1;
                    for (auto &kv : chans)
                        if (ImGui::Selectable(kv.second.c_str(), kv.first == curidx))
                            d.ch_map[c.id] = kv.first;
                    ImGui::EndCombo();
                }
                if (c.required && curidx < 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "required");
                }
            }
            ImGui::TextDisabled("Options");
            for (auto &op : d.meta.options) {
                if (op.kind == DecOption::ENUM) {
                    std::string cur = d.opts.contains(op.id) && d.opts[op.id].is_string()
                                          ? d.opts[op.id].get<std::string>()
                                          : (op.values.empty() ? "" : op.values[0]);
                    if (ImGui::BeginCombo(op.desc.c_str(), cur.c_str())) {
                        for (auto &v : op.values)
                            if (ImGui::Selectable(v.c_str(), v == cur)) d.opts[op.id] = v;
                        ImGui::EndCombo();
                    }
                } else if (op.kind == DecOption::INT) {
                    int v = 0;
                    if (d.opts.contains(op.id) && d.opts[op.id].is_number())
                        v = d.opts[op.id].get<int>();
                    if (ImGui::InputInt(op.desc.c_str(), &v)) d.opts[op.id] = v;
                } else {
                    char buf[256];
                    std::string s = d.opts.contains(op.id) && d.opts[op.id].is_string()
                                        ? d.opts[op.id].get<std::string>() : "";
                    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                    if (ImGui::InputText(op.desc.c_str(), buf, sizeof(buf))) d.opts[op.id] = std::string(buf);
                }
            }
        }
        ImGui::PopID();
    }
    if (remove >= 0) decoders_.erase(decoders_.begin() + remove);
    ImGui::End();
}

void App::draw_capture_panel() {
    ImGui::SetNextWindowPos(ImVec2(8, 730), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Capture");
    bool canStart = connected_ && sel_dev_ >= 0 && !capturing_;
    if (capturing_) {
        if (ImGui::Button("Stop")) stop_capture();
        ImGui::SameLine();
        if (meta_pending_.load() && last_cfg_triggered_) {
            // No data yet + a trigger is configured = we are armed and waiting.
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "armed - waiting for trigger...");
        } else {
            SessionMeta m = meta_copy();
            if (m.total_samples > 0) {
                float frac = (float)((double)logic_.count() / (double)m.total_samples);
                ImGui::ProgressBar(frac, ImVec2(-1, 0));
            } else {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "capturing (continuous)...");
            }
        }
    } else {
        if (!canStart) ImGui::BeginDisabled();
        if (ImGui::Button("Start")) start_capture();
        if (!canStart) ImGui::EndDisabled();
        if (!canStart) {
            ImGui::SameLine();
            ImGui::TextDisabled(!connected_ ? "(not connected)" : "(no device selected)");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Repeat", &repeat_capture_);
        ImGui::SetItemTooltip("Automatically re-arm after each complete capture");
    }
    if (!start_error_.empty())
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", start_error_.c_str());
    if (!capture_reason_.empty()) {
        if (capture_reason_.rfind("timeout", 0) == 0)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "last: %s", capture_reason_.c_str());
        else
            ImGui::Text("last: %s", capture_reason_.c_str());
    }
    ImGui::Separator();
    ImGui::Text("logic samples: %llu", (unsigned long long)logic_.count());
    ImGui::Text("annotations:   %zu", anns_.total());
    ImGui::Checkbox("Follow newest", &follow_);

    ImGui::SeparatorText("Capture file (.son)");
    ImGui::SetNextItemWidth(-34);
    ImGui::InputText("##file", save_path_, sizeof(save_path_));
    ImGui::SameLine();
    if (ImGui::SmallButton("ts")) {  // timestamped filename
        char b[64];
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(b, sizeof(b), "capture-%Y%m%d-%H%M%S.son", &tmv);
        std::snprintf(save_path_, sizeof(save_path_), "%s", b);
    }
    ImGui::SetItemTooltip("Set a timestamped file name");
    if (ImGui::Button("Save")) {
        struct stat sb;
        if (::stat(save_path_, &sb) == 0) {
            confirm_overwrite_ = true;  // exists: ask first
        } else {
            std::string e;
            if (save_capture(save_path_, e)) log(std::string("saved ") + save_path_);
            else log("save failed: " + e);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::string e;
        if (load_capture(save_path_, e)) log(std::string("loaded ") + save_path_);
        else log("load failed: " + e);
    }
    if (confirm_overwrite_) ImGui::OpenPopup("Overwrite?");
    if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s exists. Overwrite it?", save_path_);
        if (ImGui::Button("Overwrite")) {
            std::string e;
            if (save_capture(save_path_, e)) log(std::string("saved ") + save_path_);
            else log("save failed: " + e);
            confirm_overwrite_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_overwrite_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (record_truncated_)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "recording truncated at 512 MB");
    ImGui::End();
}

// Decoded-annotation table: searchable, exportable — the lane rectangles are
// unreadable at bench zoom; this is where you actually read the bytes.
void App::draw_decoded_panel() {
    ImGui::Begin("Decoded");
    // row-name lookup from DECODER_INFO
    auto row_name = [&](uint32_t stack, uint16_t row) -> std::string {
        for (auto &dr : decoder_rows_)
            if (dr.stack_id == stack)
                for (auto &r : dr.rows)
                    if (r.first == (int)row) return dr.id + "/" + r.second;
        return std::to_string(stack) + "/" + std::to_string(row);
    };

    bool filter_changed = ImGui::InputTextWithHint("##tf", "filter text",
                                                   table_filter_, sizeof(table_filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##csv", csv_path_, sizeof(csv_path_));
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        std::string e;
        if (export_annotations_csv(csv_path_, e)) log(std::string("exported ") + csv_path_);
        else log("export failed: " + e);
    }

    // Rebuild the cache lazily (and throttled while a live decode is growing).
    size_t tot = anns_.total();
    if (--table_cooldown_ < 0) table_cooldown_ = 0;
    if ((tot != table_total_seen_ && table_cooldown_ == 0) || filter_changed) {
        table_total_seen_ = tot;
        table_cooldown_ = 30;  // at most ~2 rebuilds/second while streaming
        table_cache_.clear();
        std::string f = table_filter_;
        for (auto &c : f) c = (char)tolower((unsigned char)c);
        anns_.for_each([&](uint32_t stack, uint16_t row, const Annotation &a) {
            const std::string &txt = a.texts.empty() ? std::string() : a.texts.front();
            if (!f.empty()) {
                std::string hay = txt;
                for (auto &c : hay) c = (char)tolower((unsigned char)c);
                if (hay.find(f) == std::string::npos) return;
            }
            table_cache_.push_back({a.start, a.end, stack, row, txt});
        });
        std::sort(table_cache_.begin(), table_cache_.end(),
                  [](const AnnRow &a, const AnnRow &b) { return a.start < b.start; });
    }

    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    ImGui::TextDisabled("%zu rows (double-click to jump)", table_cache_.size());
    if (ImGui::BeginTable("dec", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("row", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("value");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)table_cache_.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const AnnRow &r = table_cache_[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                bool clicked = ImGui::Selectable(fmt_secs((double)r.start / sr).c_str(), false,
                                                 ImGuiSelectableFlags_SpanAllColumns |
                                                     ImGuiSelectableFlags_AllowDoubleClick);
                if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    view_center_on((double)r.start);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row_name(r.stack, r.row).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.text.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

bool App::export_annotations_csv(const std::string &path, std::string &err) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open " + path; return false; }
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    std::fprintf(f, "start_s,end_s,start_sample,end_sample,stack,row,text\n");
    anns_.for_each([&](uint32_t stack, uint16_t row, const Annotation &a) {
        std::string txt = a.texts.empty() ? std::string() : a.texts.front();
        for (auto &c : txt)
            if (c == '"') c = '\'';
        std::fprintf(f, "%.9f,%.9f,%llu,%llu,%u,%u,\"%s\"\n", (double)a.start / sr,
                     (double)a.end / sr, (unsigned long long)a.start,
                     (unsigned long long)a.end, stack, row, txt.c_str());
    });
    std::fclose(f);
    return true;
}

void App::draw_markers_panel() {
    ImGui::Begin("Markers");
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    if (ImGui::Button("Clear all")) markers_.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("click waveform to add, drag to move");
    if (ImGui::BeginTable("mk", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("sample");
        ImGui::TableSetupColumn("time");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int del = -1;
        for (size_t i = 0; i < markers_.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableNextColumn();
            char nm[24];
            std::snprintf(nm, sizeof(nm), "%s", markers_[i].name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##n", nm, sizeof(nm))) markers_[i].name = nm;
            ImGui::TableNextColumn();
            double s = markers_[i].sample;  // numeric position entry
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##s", &s, 0, 0, "%.0f",
                                   ImGuiInputTextFlags_EnterReturnsTrue))
                markers_[i].sample = s < 0 ? 0 : s;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_secs(markers_[i].sample / sr).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("go")) view_center_on(markers_[i].sample);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) del = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del >= 0) markers_.erase(markers_.begin() + del);
    }
    ImGui::End();
}

void App::draw_measure_panel() {
    ImGui::Begin("Measurements");
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    if (markers_.size() >= 2) {
        ImGui::SeparatorText("Consecutive markers");
        for (size_t i = 1; i < markers_.size(); ++i) {
            double dt = std::fabs(markers_[i].sample - markers_[i - 1].sample) / sr;
            ImGui::Text("%s->%s: dt=%s  f=%.5g Hz", markers_[i - 1].name.c_str(),
                        markers_[i].name.c_str(), fmt_secs(dt).c_str(), dt > 0 ? 1.0 / dt : 0.0);
        }
    } else {
        ImGui::TextDisabled("add >=2 markers for deltas");
    }

    update_measurements();
    if (!meas_.lines.empty()) {
        ImGui::SeparatorText("Between last two markers");
        for (auto &l : meas_.lines) ImGui::TextUnformatted(l.c_str());
    }

    ImGui::SeparatorText("Macros (math over markers)");
    ImGui::TextDisabled("vars: m1..mN=time(s), s1..sN=sample, sr; funcs abs,log10");
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##macro", new_macro_, sizeof(new_macro_));
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        Macro mc;
        mc.name = "macro" + std::to_string(macros_.size() + 1);
        mc.expr = new_macro_;
        macros_.push_back(mc);
    }
    int del = -1;
    for (size_t i = 0; i < macros_.size(); ++i) {
        ImGui::PushID(2000 + (int)i);
        if (ImGui::SmallButton("x")) del = (int)i;
        ImGui::SameLine();
        bool ok = false;
        double v = eval_macro(macros_[i].expr, ok);
        if (ok)
            ImGui::Text("%s = %.6g   [%s]", macros_[i].name.c_str(), v, macros_[i].expr.c_str());
        else
            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s = ERR   [%s]",
                               macros_[i].name.c_str(), macros_[i].expr.c_str());
        ImGui::PopID();
    }
    if (del >= 0) macros_.erase(macros_.begin() + del);
    ImGui::End();
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        bool have = false;
        { std::lock_guard<std::mutex> lk(rec_m_); have = !record_.empty(); }
        if (ImGui::MenuItem("Save capture", nullptr, false, have)) {
            std::string e;
            if (save_capture(save_path_, e)) log(std::string("saved ") + save_path_);
            else log("save failed: " + e);
        }
        if (ImGui::MenuItem("Load capture")) {
            std::string e;
            if (load_capture(save_path_, e)) log(std::string("loaded ") + save_path_);
            else log("load failed: " + e);
        }
        ImGui::Separator();
        // Exports next to the .son path, swapping the extension.
        std::string base = save_path_;
        size_t dot = base.rfind(".son");
        if (dot != std::string::npos) base.resize(dot);
        bool have_data = logic_.count() > logic_.first_live();
        if (ImGui::MenuItem("Export VCD (logic)", nullptr, false, have_data)) {
            std::string p = base + ".vcd", e;
            if (export_vcd(p, e)) log("exported " + p);
            else log("VCD export failed: " + e);
        }
        if (ImGui::MenuItem("Export sigrok .sr (logic)", nullptr, false, have_data)) {
            std::string p = base + ".sr", e;
            if (export_sr(p, e)) log("exported " + p + " (PulseView/sigrok-cli)");
            else log(".sr export failed: " + e);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom to fit", "F")) {
            view_fit(last_canvas_w_);
            follow_ = false;
        }
        if (ImGui::MenuItem("Reset window layout")) layout_reset_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Markers")) {
        if (ImGui::MenuItem("Clear all markers")) markers_.clear();
        if (ImGui::MenuItem("Clear all macros")) macros_.clear();
        ImGui::EndMenu();
    }
    if (connected_)
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "  connected %s", host_);
    else if (conn_state_.load() == 1)
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "  connecting...");
    else
        ImGui::TextDisabled("  disconnected");
    ImGui::EndMainMenuBar();
}

void App::draw_log_panel() {
    ImGui::SetNextWindowPos(ImVec2(412, 732), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(980, 158), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log");
    ImGui::BeginChild("logscroll");
    std::vector<std::string> copy;
    {
        std::lock_guard<std::mutex> lk(m_);
        copy = log_;
    }
    for (auto &l : copy) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void App::autopilot_step() {
    if (!autopilot_) return;
    if (ap_wait_ > 0) { ap_wait_--; return; }
    switch (ap_phase_) {
        case 0:  // connect
            if (!connected_) do_connect();
            ap_phase_ = 1;
            ap_wait_ = 30;  // give scan + decoders time to arrive
            break;
        case 1:  // once catalogs are in: pick demo, enable channels, add a decoder
            if (connected_ && !devices_.empty() && !catalog_.empty()) {
                sel_dev_ = 0;
                for (int i = 0; i < (int)devices_.size(); ++i)
                    if (devices_[i].driver == "demo") sel_dev_ = i;
                int first_logic = -1;
                for (auto &ch : devices_[sel_dev_].channels) {
                    ch.enabled = true;
                    if (first_logic < 0 && ch.type == "logic") first_logic = ch.index;
                }
                chan_names_[0] = "CLK";  // demo channel renames
                chan_names_[1] = "MOSI";
                manual_rate_on_ = true;
                manual_rate_hz_ = 4000000;  // 250 ns/sample -> zooming reaches ns
                mode_idx_ = 0;
                limit_ksamples_ = 100;
                for (auto &m : catalog_) {  // add a UART decoder lane for demonstration
                    if (m.id == "uart") {
                        DecoderInstance inst;
                        inst.meta = m;
                        init_decoder_defaults(inst);
                        if (first_logic >= 0) inst.ch_map["rx"] = first_logic;
                        decoders_.push_back(inst);
                        break;
                    }
                }
                ap_phase_ = 2;
                ap_wait_ = 5;
            } else {
                ap_wait_ = 5;  // keep waiting
            }
            break;
        case 2:  // fire the capture
            start_capture();
            ap_phase_ = 3;
            ap_wait_ = 20;
            break;
        case 3:  // after capture: drop two close markers, macros, and zoom to ns
            if (!capturing_ && logic_.count() > 2000) {
                markers_.push_back({1000.0, "m1"});
                markers_.push_back({1002.0, "m2"});  // 2 samples = 500 ns @ 4 MHz
                marker_seq_ = 2;
                macros_.push_back({"period", "m2-m1"});
                macros_.push_back({"freq", "1/(m2-m1)"});
                view_start_ = 996.0;  // zoom in around the markers -> ns-scale grid
                spp_ = 0.02;
                view_init_ = true;
                follow_ = false;
                ap_phase_ = 4;
            } else {
                ap_wait_ = 5;
            }
            break;
        case 4:  // save a demo capture that includes the markers/macros/renames
            {
                std::string e;
                if (save_capture("/tmp/sonview_demo.son", e)) log("autopilot saved demo capture");
            }
            ap_phase_ = 5;
            break;
        case 5:  // exercise the exports
            {
                std::string e;
                if (export_vcd("/tmp/sonview_demo.vcd", e)) log("autopilot exported VCD");
                else log("autopilot VCD export failed: " + e);
                if (export_sr("/tmp/sonview_demo.sr", e)) log("autopilot exported .sr");
                else log("autopilot .sr export failed: " + e);
            }
            ap_phase_ = 6;
            break;
        case 6:  // exercise post-hoc re-decode over the captured data
            start_redecode();
            ap_phase_ = 7;
            ap_wait_ = 30;
            break;
        case 7:  // wait for the decode to finish (result logged by DECODE_END)
            if (redecoding_.load()) ap_wait_ = 10;
            else ap_phase_ = 8;
            break;
        default:
            break;  // done
    }
}

void App::draw_ui() {
    process_ctrl();

    // deferred first-frame connect (`sonview --connect <host>`)
    if (connect_requested_) {
        connect_requested_ = false;
        do_connect();
    }
    // auto-reconnect after an unexpected loss (~3 s between attempts)
    if (conn_lost_.load() && auto_reconnect_ && !connected_ && conn_state_.load() != 1) {
        if (--reconnect_cooldown_ <= 0) {
            reconnect_cooldown_ = 180;
            do_connect();
        }
    }

    // Space = start/stop (when not typing into a field)
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (capturing_) stop_capture();
        else if (connected_ && sel_dev_ >= 0) start_capture();
    }

    autopilot_step();
    draw_menu_bar();
    draw_connection_panel();
    draw_device_panel();
    draw_decoder_panel();
    draw_capture_panel();
    draw_markers_panel();
    draw_measure_panel();
    draw_decoded_panel();
    draw_canvas_panel();
    draw_log_panel();
}

}  // namespace son
