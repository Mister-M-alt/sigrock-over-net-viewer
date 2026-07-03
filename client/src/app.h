// sonview GUI application state and control logic.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model.h"
#include "net.h"
#include "son_wire.h"
#include "stores.h"

namespace son {

class App {
public:
    App();
    ~App();

    // Called once per frame from the main loop (inside an ImGui frame).
    void draw_ui();

    void set_default_host(const std::string &host);
    // Connect on the first frame (used by `sonview --connect <host>`).
    void request_connect() { connect_requested_ = true; }
    // Auto-connect + auto-start a demo capture (for screenshots / smoke tests).
    void enable_autopilot() { autopilot_ = true; }
    // Headless: load a .son capture file and print a summary; optionally export
    // <export_base>.vcd + .sr. Returns process code.
    int replay_and_report(const std::string &path, const std::string &export_base = "");
    bool want_quit() const { return quit_; }
    // One-shot flags for the gui loop (dock layout rebuild, window title).
    bool consume_layout_reset() {
        bool r = layout_reset_;
        layout_reset_ = false;
        return r;
    }
    std::string window_title() const;

private:
    // ---- connection / RX thread ----
    void do_connect();
    void do_disconnect();
    void rx_loop();
    void process_ctrl();  // main thread: apply queued control replies
    void start_capture();
    void stop_capture();
    void log(const std::string &s);

    // ---- panels (app.cpp) ----
    void draw_connection_panel();
    void draw_device_panel();
    void draw_decoder_panel();
    void draw_capture_panel();
    void draw_markers_panel();
    void draw_measure_panel();
    void draw_decoded_panel();
    void draw_log_panel();
    void draw_menu_bar();

    // ---- waveform canvas (render.cpp) ----
    void draw_canvas_panel();
    void draw_logic_row(void *drawlist, const ChannelInfo &ch, float x0, float x1,
                        float y_top, float y_bot);
    void draw_analog_row(void *drawlist, const ChannelInfo &ch, float x0, float x1,
                         float y_top, float y_bot);
    void draw_ann_rows(void *drawlist, float x0, float x1, float &y);
    void draw_time_grid(void *drawlist, float x0, float x1, float y0, float y1);
    void draw_markers(void *drawlist, float x0, float x1, float y0, float y1);

    // sample<->x mapping helpers (canvas coords)
    double x_of(double sample, float x0) const { return x0 + (sample - view_start_) / spp_; }
    double sample_of(double x, float x0) const { return view_start_ + (x - x0) * spp_; }
    void clamp_view(float canvas_w);
    void view_fit(float canvas_w);
    void view_center_on(double sample);
    // Nearest signal edge of `bit` within +-window samples of s (marker snap).
    bool nearest_edge(int bit, double s, double window, double &edge) const;
    SessionMeta meta_copy();

    // ---- members ----
    Client client_;
    std::thread rx_thread_;
    std::atomic<bool> rx_stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> capturing_{false};
    std::atomic<bool> need_view_reset_{false};

    // connection lifecycle (async connect + auto-reconnect)
    std::atomic<int> conn_state_{0};       // 0 idle, 1 connecting, 2 failed
    std::atomic<bool> conn_lost_{false};   // unexpected loss -> auto-reconnect
    bool auto_reconnect_ = true;
    int reconnect_cooldown_ = 0;           // frames until next attempt
    bool connect_requested_ = false;
    bool ui_was_connected_ = false;        // rising-edge save of host/port
    std::string conn_err_;                 // guarded by m_

    // deferred capture start: nothing is wiped until the first data arrives, so
    // an armed capture that never triggers cannot destroy the previous capture.
    SessionMeta pending_meta_;             // guarded by m_
    std::atomic<bool> meta_pending_{false};
    uint64_t pending_window_ = 0;          // rolling window to apply at commit
    bool last_cfg_triggered_ = false;      // capture was armed with a trigger
    void commit_pending_capture();         // RX thread, at first data
    void swap_buffers_on_end();            // RX thread, at CAPTURE_END

    // trigger-fired marker (SON_MSG_TRIGGER)
    std::atomic<bool> has_trigger_{false};
    std::atomic<uint64_t> trigger_sample_{0};

    std::mutex m_;                 // guards ctrl_, meta_, log_
    std::vector<son_msg> ctrl_;    // control replies queued by RX thread
    SessionMeta meta_;
    bool meta_valid_ = false;

    // Capture storage is double-buffered, like a scope's acquisition memory:
    // in Repeat mode the next acquisition fills the hidden buffer while the
    // display keeps showing the previous one, and the buffers swap atomically
    // at CAPTURE_END — no blink, no half-drawn traces. Outside Repeat mode
    // live == fill and data streams progressively exactly as before.
    struct CaptureBuf {
        LogicStore logic;
        std::map<uint32_t, AnalogStore *> analog;  // guarded by m_ (structure)
        AnnotationStore anns;
    };
    std::unique_ptr<CaptureBuf> bufs_[2];  // heap: LogicStore is ~0.5 MB
    std::atomic<int> live_idx_{0};         // render thread reads this buffer
    int fill_idx_ = 0;                     // RX thread writes this buffer
    bool swap_pending_ = false;            // RX-owned: swap at CAPTURE_END
    SessionMeta fill_meta_;                // RX-owned: published at swap
    uint64_t staged_trigger_ = 0;          // RX-owned trigger of the fill capture
    bool staged_has_trigger_ = false;

    LogicStore &logic() { return bufs_[live_idx_.load(std::memory_order_acquire)]->logic; }
    const LogicStore &logic() const {
        return bufs_[live_idx_.load(std::memory_order_acquire)]->logic;
    }
    AnnotationStore &anns() { return bufs_[live_idx_.load(std::memory_order_acquire)]->anns; }
    const AnnotationStore &anns() const {
        return bufs_[live_idx_.load(std::memory_order_acquire)]->anns;
    }
    LogicStore &logic_fill() { return bufs_[fill_idx_]->logic; }
    AnnotationStore &anns_fill() { return bufs_[fill_idx_]->anns; }
    AnalogStore *analog_get(uint32_t channel_id);        // live, read-only, under m_
    AnalogStore *analog_fill_get(uint32_t channel_id);   // fill, creates, under m_

    // scope-style run state: true from Start until Stop / terminal end, so the
    // Run/Stop button never flip-flops between repeat acquisitions.
    std::atomic<bool> run_active_{false};
    int acq_count_ = 0;  // acquisitions since Start (shown in the Capture panel)

    // connection UI
    char host_[256] = "127.0.0.1";
    int port_ = SON_DEFAULT_PORT;
    std::string server_info_;
    std::string capture_reason_;

    // device / decoder catalogs (main thread)
    std::vector<DeviceInfo> devices_;
    int sel_dev_ = -1;
    std::vector<DecoderMeta> catalog_;
    std::vector<DecoderInstance> decoders_;
    std::vector<DecoderRowInfo> decoder_rows_;
    int add_decoder_idx_ = 0;

    // user channel renames (channel index -> custom label); applied everywhere.
    std::map<int, std::string> chan_names_;
    std::string chan_name(int index, const std::string &def) const;

    // capture config
    int samplerate_idx_ = 0;
    int manual_rate_hz_ = 1000000;
    bool manual_rate_on_ = false;
    int mode_idx_ = 0;          // 0 = triggered, 1 = continuous
    int limit_ksamples_ = 100;  // *1000 samples
    int capture_ratio_ = 0;     // pre-trigger %, 0 = none
    bool repeat_capture_ = false;  // auto re-arm after a complete capture
    bool follow_ = true;
    int max_window_msamples_ = 20;  // rolling window (millions), continuous
    std::string start_error_;   // last validation error, shown in Capture panel

    // settings persistence (~/.config/sonview/config.json)
    void load_settings();
    void save_settings();
    std::string settings_path() const;
    static std::string dev_key(const DeviceInfo &d);
    void harvest_device_prefs();               // selected device -> device_prefs_
    void apply_device_prefs(DeviceInfo &d);    // device_prefs_ -> device GUI state
    json device_prefs_ = json::object();       // per-device persisted GUI state
    std::string sel_dev_key_;                  // identity of the selected device

    // view state
    double view_start_ = 0;   // leftmost visible sample
    double spp_ = 64.0;       // samples per pixel
    bool view_init_ = false;
    std::atomic<bool> user_view_touched_{false};  // pan/zoom since capture start
    float last_canvas_w_ = 1000.0f;
    // While a capture is streaming in, clamp the view against the EXPECTED
    // total so a held zoom position isn't dragged back to sample 0 before the
    // data reaches it. 0 = clamp by received data only.
    double view_total_hint_ = 0;
    // markers (N named vertical markers) + macros (expressions over marker times)
    struct Marker { double sample; std::string name; };
    std::vector<Marker> markers_;
    int marker_seq_ = 0;
    int drag_marker_ = -1;   // marker being dragged, or -1
    struct Macro { std::string name; std::string expr; };
    std::vector<Macro> macros_;
    char new_macro_[128] = "1/(m2-m1)";
    double render_sr_ = 0;   // samplerate used by the grid/marker readouts
    double eval_macro(const std::string &expr, bool &ok) const;

    std::vector<std::string> log_;
    bool quit_ = false;
    bool layout_reset_ = false;

    // decoded-annotation table (rebuilt lazily from anns_)
    struct AnnRow {
        uint64_t start, end;
        uint32_t stack;
        uint16_t row;
        std::string text;
    };
    std::vector<AnnRow> table_cache_;
    size_t table_total_seen_ = (size_t)-1;
    int table_cooldown_ = 0;
    char table_filter_[64] = "";
    char csv_path_[256] = "decoded.csv";
    bool export_annotations_csv(const std::string &path, std::string &err);

    // decoder picker filter
    char dec_filter_[48] = "";

    // save-overwrite confirmation
    bool confirm_overwrite_ = false;

    // autopilot (screenshots / smoke test): connect -> pick demo -> capture.
    bool autopilot_ = false;
    int ap_phase_ = 0;
    int ap_wait_ = 0;
    void autopilot_step();

    // capture recording (.son = recorded protocol frames) + save/load
    std::mutex rec_m_;
    std::vector<uint8_t> record_;         // committed capture frames
    std::vector<uint8_t> pending_record_; // frames since SESSION_META, pre-commit
    bool pending_rec_active_ = false;     // guarded by rec_m_
    bool record_truncated_ = false;
    char save_path_[256] = "capture.son";
    void process_rx_message(son_msg &msg);  // shared by RX thread and file replay
    void record_frame(const son_msg &msg);
    bool save_capture(const std::string &path, std::string &err);
    bool load_capture(const std::string &path, std::string &err);
    std::string client_state_dump() const;  // markers+macros+renames as JSON
    void apply_client_state(const std::string &js);

    // exports (export.cpp) + post-hoc re-decode of the captured data
    bool export_vcd(const std::string &path, std::string &err);
    bool export_sr(const std::string &path, std::string &err);
    void start_redecode();
    std::atomic<bool> redecoding_{false};

    // between-marker measurements (cached; recomputed when inputs change)
    struct MeasCache {
        double a = -1, b = -1;
        uint64_t count = 0;
        std::vector<std::string> lines;
    } meas_;
    void update_measurements();

    // channel display order (per capture; persisted per device)
    std::vector<int> chan_order_;
    std::vector<const ChannelInfo *> ordered_channels(const SessionMeta &m) const;

    // oscilloscope view: analog channels on a shared graticule, per-channel
    // vertical scale/offset like a bench scope (persisted per device)
    struct ScopeChan {
        bool show = true;
        bool ac = false;       // AC coupling: subtract the visible-window mean
        bool inited = false;   // auto-fit once when data first appears
        float vdiv = 1.0f;     // volts per division
        float voff = 0.0f;     // voltage at the vertical center
    };
    std::map<int, ScopeChan> scope_;
    // digital channels on the scope (MSO style): position + size in divisions
    struct ScopeDig {
        bool show = true;
        bool inited = false;   // auto-stacked once on first draw
        float pos = 0.0f;      // low level, in divisions above center
        float size = 0.3f;     // trace height, in divisions
    };
    std::map<int, ScopeDig> scope_d_;
    bool show_scope_ = true;
    bool show_waveform_ = true;     // at least one of the two stays visible
    bool scope_trig_drag_ = false;  // dragging the trigger-position flag
    void draw_scope_panel();   // render.cpp
    void scope_autofit(int chan_index, ScopeChan &sc);
};

}  // namespace son
