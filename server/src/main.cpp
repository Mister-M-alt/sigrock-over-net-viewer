// sond — sigrok-over-net server. Control plane, device scan, decoder enumeration,
// and the TCP accept/dispatch loop. Capture/streaming lives in capture.cpp.

#include "server.h"
#include "son_wire.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

struct sr_context *g_ctx = nullptr;
std::vector<DeviceEntry> g_devices;
std::atomic<struct sr_session *> g_active_session{nullptr};
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_capture_running{false};
std::atomic<bool> g_first_data{false};
std::atomic<bool> g_watchdog_fired{false};

DeviceEntry *find_device(int id)
{
    for (auto &d : g_devices)
        if (d.id == id)
            return &d;
    return nullptr;
}

static const char *chan_type_str(int t)
{
    return t == SR_CHANNEL_LOGIC ? "logic" : t == SR_CHANNEL_ANALOG ? "analog" : "?";
}

// -------- GVariant helpers ---------------------------------------------------

static json gvariant_to_json(GVariant *v)
{
    if (!v)
        return nullptr;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
        return std::string(g_variant_get_string(v, nullptr));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
        return (int64_t)g_variant_get_int64(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
        return (uint64_t)g_variant_get_uint64(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
        return (int64_t)g_variant_get_int32(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        return (uint64_t)g_variant_get_uint32(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        return g_variant_get_double(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        return (bool)g_variant_get_boolean(v);
    gchar *s = g_variant_print(v, FALSE);
    json r = std::string(s ? s : "");
    g_free(s);
    return r;
}

// Best-effort list of samplerates (Hz) a device supports.
static std::vector<uint64_t> device_samplerates(struct sr_dev_driver *drv, struct sr_dev_inst *sdi)
{
    std::vector<uint64_t> out;
    GVariant *dict = nullptr;
    if (sr_config_list(drv, sdi, nullptr, SR_CONF_SAMPLERATE, &dict) != SR_OK || !dict)
        return out;

    GVariant *list = g_variant_lookup_value(dict, "samplerates", G_VARIANT_TYPE("at"));
    if (list) {
        gsize n = 0;
        const uint64_t *a = (const uint64_t *)g_variant_get_fixed_array(list, &n, sizeof(uint64_t));
        for (gsize i = 0; i < n; i++)
            out.push_back(a[i]);
        g_variant_unref(list);
    } else if ((list = g_variant_lookup_value(dict, "samplerate-steps", G_VARIANT_TYPE("at")))) {
        gsize n = 0;
        const uint64_t *a = (const uint64_t *)g_variant_get_fixed_array(list, &n, sizeof(uint64_t));
        if (n == 3) {
            uint64_t lo = a[0], hi = a[1];
            static const uint64_t nice[] = {
                1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
                1000000, 2000000, 5000000, 10000000, 12000000, 24000000, 50000000, 100000000};
            out.push_back(lo);
            for (uint64_t r : nice)
                if (r > lo && r <= hi)
                    out.push_back(r);
        }
        g_variant_unref(list);
    }
    g_variant_unref(dict);
    return out;
}

// -------- Scan ---------------------------------------------------------------

json scan_devices(bool rescan)
{
    if (rescan && !g_capture_running.load()) {
        // Drop cached instances so hot-plugged hardware is discovered.
        std::set<struct sr_dev_driver *> drvs;
        for (auto &d : g_devices)
            drvs.insert(d.drv);
        for (auto *drv : drvs)
            sr_dev_clear(drv);
        g_devices.clear();
    }
    if (g_devices.empty()) {
        struct sr_dev_driver **drivers = sr_driver_list(g_ctx);
        int next_id = 0;
        for (int i = 0; drivers && drivers[i]; i++) {
            struct sr_dev_driver *drv = drivers[i];
            if (sr_driver_init(g_ctx, drv) != SR_OK)
                continue;
            GSList *devs = sr_driver_scan(drv, nullptr);
            for (GSList *l = devs; l; l = l->next)
                g_devices.push_back({next_id++, drv, (struct sr_dev_inst *)l->data});
            g_slist_free(devs);
        }
    }

    json out;
    out["devices"] = json::array();
    for (auto &d : g_devices) {
        json jd;
        jd["id"] = d.id;
        jd["driver"] = d.drv->name;
        const char *v = sr_dev_inst_vendor_get(d.sdi);
        const char *m = sr_dev_inst_model_get(d.sdi);
        const char *c = sr_dev_inst_connid_get(d.sdi);
        jd["vendor"] = v ? v : "";
        jd["model"] = m ? m : "";
        jd["conn"] = c ? c : "";
        jd["samplerates"] = device_samplerates(d.drv, d.sdi);
        // Honest trigger capability: drivers without SR_CONF_TRIGGER_MATCH
        // (e.g. ftdi-la) silently ignore triggers — don't pretend otherwise.
        {
            GVariant *tm = nullptr;
            bool has_trig =
                sr_config_list(d.drv, d.sdi, nullptr, SR_CONF_TRIGGER_MATCH, &tm) == SR_OK;
            if (tm) g_variant_unref(tm);
            jd["triggers"] = has_trig;
        }
        jd["channels"] = json::array();
        for (GSList *ch = sr_dev_inst_channels_get(d.sdi); ch; ch = ch->next) {
            struct sr_channel *c2 = (struct sr_channel *)ch->data;
            jd["channels"].push_back({{"index", c2->index},
                                      {"name", c2->name ? c2->name : ""},
                                      {"type", chan_type_str(c2->type)}});
        }
        out["devices"].push_back(jd);
    }
    return out;
}

// -------- Decoder enumeration (pure C struct reads; no GIL needed) -----------

json list_decoders_json()
{
    json out;
    out["decoders"] = json::array();
    for (const GSList *dl = srd_decoder_list(); dl; dl = dl->next) {
        struct srd_decoder *d = (struct srd_decoder *)dl->data;
        json jd;
        jd["id"] = d->id ? d->id : "";
        jd["name"] = d->name ? d->name : "";
        jd["longname"] = d->longname ? d->longname : "";
        jd["channels"] = json::array();
        auto add_ch = [&](GSList *list, bool required) {
            for (GSList *c = list; c; c = c->next) {
                struct srd_channel *ch = (struct srd_channel *)c->data;
                jd["channels"].push_back({{"id", ch->id ? ch->id : ""},
                                          {"name", ch->name ? ch->name : ""},
                                          {"desc", ch->desc ? ch->desc : ""},
                                          {"required", required}});
            }
        };
        add_ch(d->channels, true);
        add_ch(d->opt_channels, false);

        jd["options"] = json::array();
        for (GSList *o = d->options; o; o = o->next) {
            struct srd_decoder_option *op = (struct srd_decoder_option *)o->data;
            json jo;
            jo["id"] = op->id ? op->id : "";
            jo["desc"] = op->desc ? op->desc : "";
            jo["default"] = gvariant_to_json(op->def);
            jo["values"] = json::array();
            for (GSList *vv = op->values; vv; vv = vv->next)
                jo["values"].push_back(gvariant_to_json((GVariant *)vv->data));
            jd["options"].push_back(jo);
        }

        // Each annotation class is a char*[] { name, desc } (as PulseView reads it).
        jd["classes"] = json::array();
        int idx = 0;
        for (GSList *a = d->annotations; a; a = a->next, idx++) {
            char **ann = (char **)a->data;
            const char *nm = (ann && ann[0]) ? ann[0] : "";
            const char *ds = (ann && ann[0] && ann[1]) ? ann[1] : "";
            jd["classes"].push_back({{"index", idx}, {"name", nm}, {"desc", ds}});
        }

        jd["rows"] = json::array();
        int rid = 0;
        for (GSList *r = d->annotation_rows; r; r = r->next, rid++) {
            struct srd_decoder_annotation_row *row = (struct srd_decoder_annotation_row *)r->data;
            json jr;
            jr["id"] = row->id ? row->id : "";
            jr["name"] = row->desc ? row->desc : (row->id ? row->id : "");
            jr["row_id"] = rid;
            jr["classes"] = json::array();
            for (GSList *cc = row->ann_classes; cc; cc = cc->next)
                jr["classes"].push_back(GPOINTER_TO_INT(cc->data));
            jd["rows"].push_back(jr);
        }
        out["decoders"].push_back(jd);
    }
    return out;
}

// -------- CONFIG parsing -----------------------------------------------------

static std::vector<DecoderCfg> parse_decoders(const json &arr)
{
    std::vector<DecoderCfg> out;
    for (auto &d : arr) {
        DecoderCfg dc;
        dc.id = d.value("id", std::string());
        if (d.contains("channels"))
            for (auto it = d["channels"].begin(); it != d["channels"].end(); ++it)
                dc.channels[it.key()] = it.value().get<int>();
        if (d.contains("options"))
            dc.options = d["options"];
        out.push_back(std::move(dc));
    }
    return out;
}

static CaptureCfg parse_config(const json &j)
{
    CaptureCfg c;
    c.device_id = j.value("device_id", -1);
    c.samplerate = j.value("samplerate", (uint64_t)1000000);
    c.mode = j.value("mode", std::string("triggered"));
    c.limit_samples = j.value("limit_samples", (uint64_t)1000000);
    c.capture_ratio = j.value("capture_ratio", 0);
    if (j.contains("channels"))
        for (auto &ch : j["channels"])
            c.channels.push_back(ch.get<int>());
    if (j.contains("triggers"))
        for (auto &t : j["triggers"])
            c.triggers.emplace_back(t.value("channel", 0), t.value("match", std::string("rising")));
    if (j.contains("decoders"))
        c.decoders = parse_decoders(j["decoders"]);
    return c;
}

// -------- Client handling ----------------------------------------------------

// Wait until `fd` has data. Returns false if the listen socket has a pending
// connection instead (new-client-wins: a fresh sonview replaces a stale one,
// e.g. after a silent network partition left this socket half-open).
static bool wait_readable(int fd, int srv)
{
    struct pollfd p[2] = {{fd, POLLIN, 0}, {srv, POLLIN, 0}};
    for (;;) {
        int rc = poll(p, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (p[1].revents & POLLIN)
            return false; // yield to the new client
        if (p[0].revents)
            return true;
    }
}

static void handle_client(int fd, int srv)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Detect silently-dead peers (cable pull, wifi drop) in ~15s so the accept
    // loop can never be wedged by a half-open connection.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 5, intvl = 3, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    CaptureCfg pending;
    std::thread cap;
    // Stop a running capture and reclaim its thread; if it's truly stuck (the
    // driver ignored sr_session_stop), detach it so we never block accepting
    // new clients. Leaves g_active_session cleared so a fresh capture can start.
    auto reap = [&]() {
        if (!cap.joinable())
            return;
        g_stop_requested = true;
        if (auto s = g_active_session.load())
            sr_session_stop(s);
        for (int i = 0; i < 300 && g_capture_running.load(); ++i)
            usleep(10000);  // wait up to ~3s for a graceful stop
        if (g_capture_running.load()) {
            fprintf(stderr, "[server] capture stuck; detaching thread\n");
            cap.detach();
            g_active_session = nullptr;
        } else {
            cap.join();
        }
    };
    son_msg m;
    void *decode_state = nullptr;  // active post-hoc decode session
    int decode_unitsize = 1;
    while (wait_readable(fd, srv) && son_recv(fd, m)) {
        switch (m.hdr.type) {
        case SON_MSG_DECODE_REQ: {
            if (decode_state) { decode_end(decode_state, fd); decode_state = nullptr; }
            try {
                json j = json::parse(m.json());
                uint64_t sr = j.value("samplerate", (uint64_t)1000000);
                decode_unitsize = j.value("unitsize", 1);
                std::vector<DecoderCfg> decs =
                    j.contains("decoders") ? parse_decoders(j["decoders"])
                                           : std::vector<DecoderCfg>{};
                decode_state = decode_begin(fd, sr, decs);
                if (!decode_state) {
                    json e = {{"code", "decode"}, {"message", "decoder setup failed"}};
                    son_send_json(fd, SON_MSG_ERROR, e.dump());
                }
            } catch (const std::exception &e) {
                json err = {{"code", "decode"}, {"message", e.what()}};
                son_send_json(fd, SON_MSG_ERROR, err.dump());
            }
            break;
        }
        case SON_MSG_DATA_LOGIC: {
            // Client -> server only happens during a decode session.
            if (!decode_state) break;
            if (m.payload.size() >= sizeof(son_logic_hdr)) {
                son_logic_hdr h;
                memcpy(&h, m.payload.data(), sizeof(h));
                size_t need = (size_t)h.sample_count * h.unitsize;
                if (m.payload.size() >= sizeof(son_logic_hdr) + need)
                    decode_feed(decode_state, h.start_sample,
                                m.payload.data() + sizeof(son_logic_hdr), need,
                                h.unitsize ? h.unitsize : decode_unitsize);
            }
            break;
        }
        case SON_MSG_DECODE_END:
            if (decode_state) {
                decode_end(decode_state, fd);  // also replies DECODE_END
                decode_state = nullptr;
            }
            break;
        case SON_MSG_HELLO: {
            json r = {{"server", "sond"},
                      {"proto", SON_PROTO_VERSION},
                      {"libsigrok", sr_package_version_string_get()},
                      {"libsigrokdecode", srd_package_version_string_get()}};
            son_send_json(fd, SON_MSG_SERVER_INFO, r.dump());
            break;
        }
        case SON_MSG_SCAN_REQ: {
            bool rescan = false;
            try {
                json j = json::parse(m.json());
                rescan = j.value("rescan", false);
            } catch (...) {}
            son_send_json(fd, SON_MSG_SCAN_RESULT, scan_devices(rescan).dump());
            break;
        }
        case SON_MSG_DECODERS_REQ:
            son_send_json(fd, SON_MSG_DECODERS_LIST, list_decoders_json().dump());
            break;
        case SON_MSG_CONFIG:
            try {
                pending = parse_config(json::parse(m.json()));
            } catch (const std::exception &e) {
                json err = {{"code", "config"}, {"message", e.what()}};
                son_send_json(fd, SON_MSG_ERROR, err.dump());
            }
            break;
        case SON_MSG_START:
            if (g_capture_running.load())
                break; // already capturing
            reap(); // reclaim a finished/stuck previous thread
            g_stop_requested = false;
            {
                CaptureCfg cfg = pending;
                cap = std::thread([fd, cfg] { run_capture(fd, cfg); });
            }
            break;
        case SON_MSG_STOP: {
            g_stop_requested = true;
            auto s = g_active_session.load();
            if (s)
                sr_session_stop(s);
            break;
        }
        default:
            break;
        }
    }

    // Client gone: free any decode session, stop any running capture (detach
    // if stuck). The trailing DECODE_END write just fails on the dead socket.
    if (decode_state)
        decode_end(decode_state, fd);
    reap();
}

static int run_server(int port)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 1) < 0) { perror("listen"); return 1; }
    printf("sond listening on :%d\n", port);
    fflush(stdout);
    for (;;) {
        struct sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int fd = accept(srv, (struct sockaddr *)&ca, &cl);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        printf("client connected: %s\n", inet_ntoa(ca.sin_addr));
        fflush(stdout);
        handle_client(fd, srv);
        close(fd);
        printf("client disconnected\n");
        fflush(stdout);
    }
    close(srv);
    return 0;
}

int main(int argc, char **argv)
{
    int port = SON_DEFAULT_PORT;
    bool listen_mode = false, dbg_scan = false, dbg_dec = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen")) {
            listen_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--scan")) {
            dbg_scan = true;
        } else if (!strcmp(argv[i], "--decoders")) {
            dbg_dec = true;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: sond [--listen [port]] [--scan] [--decoders]\n");
            return 0;
        }
    }

    // A client disconnecting mid-capture must not kill us on the next socket
    // write; handle write errors via return codes instead of SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    printf("sond (sigrok-over-net server) — protocol v%d\n", SON_PROTO_VERSION);
    printf("libsigrok %s / libsigrokdecode %s\n",
           sr_package_version_string_get(), srd_package_version_string_get());

    if (sr_init(&g_ctx) != SR_OK) { fprintf(stderr, "sr_init failed\n"); return 1; }
    if (srd_init(nullptr) != SRD_OK) { fprintf(stderr, "srd_init failed\n"); return 1; }
    srd_decoder_load_all();
    // Note: srd_init() already releases the Python GIL. We must NOT call
    // PyEval_SaveThread() again here (that segfaults). Each capture thread's
    // srd_* calls re-acquire the GIL via PyGILState_Ensure (see GILGuard).

    if (dbg_scan) {
        printf("%s\n", scan_devices().dump(2).c_str());
    }
    if (dbg_dec) {
        json d = list_decoders_json();
        printf("decoders: %zu\n", d["decoders"].size());
    }

    int rc = 0;
    if (listen_mode || (!dbg_scan && !dbg_dec))
        rc = run_server(port);

    return rc;
}
