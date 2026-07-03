// capture.cpp — one capture run: configure the libsigrok device, stream logic/
// analog datafeed over the socket, and (optionally) run libsigrokdecode decoders
// whose annotations are batched back to the client.

#include "server.h"
#include "son_wire.h"

#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

// IMPORTANT: libsigrokdecode manages the Python GIL internally and dispatches
// each decoder to its own worker thread. The caller must therefore call srd_*
// WITHOUT holding the GIL — holding it deadlocks the decoder worker (which blocks
// in PyGILState_Ensure while we block in srd_session_send's g_cond_wait).

namespace {

struct AnnEntry {
    uint64_t start, end;
    int ann_class;
    std::vector<std::string> texts;
};

struct CapState {
    int fd = -1;
    uint64_t samplerate = 0;
    int unitsize = 1;
    uint64_t logic_start = 0;
    std::map<int, uint64_t> analog_start;             // channel index -> sample counter
    struct srd_session *srd_sess = nullptr;
    std::map<struct srd_decoder_inst *, int> di_stack; // instance -> stack id
    std::map<int, std::map<int, int>> class_row;       // stack -> (ann class -> row)
    std::map<std::pair<int, int>, std::vector<AnnEntry>> anns; // (stack,row) -> entries
};

void append(std::vector<uint8_t> &b, const void *p, size_t n)
{
    const uint8_t *c = (const uint8_t *)p;
    b.insert(b.end(), c, c + n);
}

// Send a data-plane payload, zstd-compressed if that meaningfully shrinks it
// (adaptive: incompressible data goes raw). Client decompresses via SON_FLAG_ZSTD.
void send_maybe_zstd(int fd, uint8_t type, const uint8_t *payload, size_t len)
{
    size_t bound = ZSTD_compressBound(len);
    std::vector<uint8_t> comp(bound);
    size_t csz = ZSTD_compress(comp.data(), bound, payload, len, 1);
    if (!ZSTD_isError(csz) && csz + 64 < len)
        son_send(fd, type, SON_FLAG_ZSTD, 1, comp.data(), (uint32_t)csz);
    else
        son_send(fd, type, 0, 1, payload, (uint32_t)len);
}

void flush_anns(CapState *st)
{
    for (auto &kv : st->anns) {
        auto &vec = kv.second;
        if (vec.empty())
            continue;
        std::vector<uint8_t> buf;
        son_ann_batch_hdr bh;
        bh.decode_stack_id = (uint32_t)kv.first.first;
        bh.row_id = (uint16_t)kv.first.second;
        bh.count = (uint16_t)std::min<size_t>(vec.size(), 65535);
        append(buf, &bh, sizeof(bh));
        size_t emitted = 0;
        for (auto &e : vec) {
            if (emitted >= bh.count)
                break;
            son_ann a;
            a.start_sample = e.start;
            a.end_sample = e.end;
            a.ann_class = (uint16_t)e.ann_class;
            a.n_texts = (uint16_t)std::min<size_t>(e.texts.size(), 65535);
            memset(a._pad, 0, sizeof(a._pad));
            append(buf, &a, sizeof(a));
            for (size_t i = 0; i < a.n_texts; i++) {
                uint16_t len = (uint16_t)std::min<size_t>(e.texts[i].size(), 65535);
                append(buf, &len, sizeof(len));
                append(buf, e.texts[i].data(), len);
            }
            emitted++;
        }
        send_maybe_zstd(st->fd, SON_MSG_ANN_BATCH, buf.data(), buf.size());
        vec.clear();
    }
}

void ann_cb(struct srd_proto_data *pd, void *cbdata)
{
    CapState *st = (CapState *)cbdata;
    if (!pd || !pd->pdo || !pd->data)
        return;
    auto it = st->di_stack.find(pd->pdo->di);
    if (it == st->di_stack.end())
        return;
    int stack = it->second;
    auto *a = (struct srd_proto_data_annotation *)pd->data;
    int row = 0;
    auto &cr = st->class_row[stack];
    auto rit = cr.find(a->ann_class);
    if (rit != cr.end())
        row = rit->second;
    AnnEntry e;
    e.start = pd->start_sample;
    e.end = pd->end_sample;
    e.ann_class = a->ann_class;
    for (char **t = a->ann_text; t && *t; ++t)
        e.texts.push_back(*t);
    st->anns[{stack, row}].push_back(std::move(e));
}

void datafeed_cb(const struct sr_dev_inst *, const struct sr_datafeed_packet *pkt, void *cbdata)
{
    CapState *st = (CapState *)cbdata;
    switch (pkt->type) {
    case SR_DF_TRIGGER:
        // Soft/hard trigger fired: everything from here on is post-trigger.
        son_send_json(st->fd, SON_MSG_TRIGGER,
                      json{{"sample", st->logic_start}}.dump());
        break;
    case SR_DF_LOGIC: {
        g_first_data.store(true, std::memory_order_relaxed);
        auto *lg = (const struct sr_datafeed_logic *)pkt->payload;
        int us = (int)lg->unitsize ? (int)lg->unitsize : 1;
        uint64_t cnt = lg->length / us;
        son_logic_hdr lh;
        lh.start_sample = st->logic_start;
        lh.sample_count = (uint32_t)cnt;
        lh.samples_dropped = 0;
        lh.unitsize = (uint8_t)us;
        memset(lh._pad, 0, sizeof(lh._pad));
        std::vector<uint8_t> pl(sizeof(lh) + lg->length);
        memcpy(pl.data(), &lh, sizeof(lh));
        memcpy(pl.data() + sizeof(lh), lg->data, lg->length);
        send_maybe_zstd(st->fd, SON_MSG_DATA_LOGIC, pl.data(), pl.size());
        if (st->srd_sess) {
            srd_session_send(st->srd_sess, st->logic_start, st->logic_start + cnt,
                             (const uint8_t *)lg->data, lg->length, (uint64_t)us);
            flush_anns(st);
        }
        st->logic_start += cnt;
        break;
    }
    case SR_DF_ANALOG: {
        g_first_data.store(true, std::memory_order_relaxed);
        auto *an = (const struct sr_datafeed_analog *)pkt->payload;
        if (!an->meaning)
            break;
        int nch = g_slist_length(an->meaning->channels);
        uint32_t ns = an->num_samples;
        if (nch <= 0 || ns == 0)
            break;
        std::vector<float> buf((size_t)ns * nch);
        if (sr_analog_to_float(an, buf.data()) != SR_OK)
            break;
        int ci = 0;
        for (GSList *c = an->meaning->channels; c; c = c->next, ci++) {
            struct sr_channel *ch = (struct sr_channel *)c->data;
            std::vector<float> col(ns);
            for (uint32_t s = 0; s < ns; s++)
                col[s] = buf[(size_t)s * nch + ci];
            son_analog_hdr ah;
            ah.start_sample = st->analog_start[ch->index];
            ah.sample_count = ns;
            ah.channel_id = (uint32_t)ch->index;
            std::vector<uint8_t> pl(sizeof(ah) + (size_t)ns * sizeof(float));
            memcpy(pl.data(), &ah, sizeof(ah));
            memcpy(pl.data() + sizeof(ah), col.data(), (size_t)ns * sizeof(float));
            send_maybe_zstd(st->fd, SON_MSG_DATA_ANALOG, pl.data(), pl.size());
            st->analog_start[ch->index] += ns;
        }
        break;
    }
    default:
        break;
    }
}

struct sr_channel *channel_by_index(struct sr_dev_inst *sdi, int index)
{
    for (GSList *c = sr_dev_inst_channels_get(sdi); c; c = c->next) {
        struct sr_channel *ch = (struct sr_channel *)c->data;
        if (ch->index == index)
            return ch;
    }
    return nullptr;
}

int trigger_match_from_str(const std::string &s)
{
    if (s == "0") return SR_TRIGGER_ZERO;
    if (s == "1") return SR_TRIGGER_ONE;
    if (s == "rising") return SR_TRIGGER_RISING;
    if (s == "falling") return SR_TRIGGER_FALLING;
    if (s == "edge") return SR_TRIGGER_EDGE;
    return 0;
}

GVariant *json_to_gvariant(const json &v)
{
    if (v.is_string()) return g_variant_new_string(v.get<std::string>().c_str());
    if (v.is_boolean()) return g_variant_new_boolean(v.get<bool>());
    if (v.is_number_integer() || v.is_number_unsigned()) return g_variant_new_int64(v.get<int64_t>());
    if (v.is_number_float()) return g_variant_new_double(v.get<double>());
    return nullptr;
}

void setup_decoders(int fd, const CaptureCfg &cfg, CapState &st)
{
    if (srd_session_new(&st.srd_sess) != SRD_OK) {
        st.srd_sess = nullptr;
        return;
    }
    int stackid = 0;
    for (auto &dc : cfg.decoders) {
        GHashTable *opts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 (GDestroyNotify)g_variant_unref);
        if (dc.options.is_object()) {
            for (auto it = dc.options.begin(); it != dc.options.end(); ++it) {
                GVariant *gv = json_to_gvariant(it.value());
                if (gv)
                    g_hash_table_insert(opts, g_strdup(it.key().c_str()), g_variant_ref_sink(gv));
            }
        }
        struct srd_decoder_inst *di = srd_inst_new(st.srd_sess, dc.id.c_str(), opts);
        g_hash_table_destroy(opts);
        if (!di)
            continue;

        GHashTable *chmap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify)g_variant_unref);
        for (auto &kv : dc.channels)
            g_hash_table_insert(chmap, g_strdup(kv.first.c_str()),
                                g_variant_ref_sink(g_variant_new_int32(kv.second)));
        srd_inst_channel_set_all(di, chmap);
        g_hash_table_destroy(chmap);

        st.di_stack[di] = stackid;
        struct srd_decoder *d = di->decoder;
        int rid = 0;
        for (GSList *r = d->annotation_rows; r; r = r->next, rid++) {
            auto *row = (struct srd_decoder_annotation_row *)r->data;
            for (GSList *cc = row->ann_classes; cc; cc = cc->next)
                st.class_row[stackid][GPOINTER_TO_INT(cc->data)] = rid;
        }
        stackid++;
    }
    srd_session_metadata_set(st.srd_sess, SRD_CONF_SAMPLERATE, g_variant_new_uint64(st.samplerate));
    srd_pd_output_callback_add(st.srd_sess, SRD_OUTPUT_ANN, ann_cb, &st);
    srd_session_start(st.srd_sess);

    // Tell the client the row/class layout so it can label annotation lanes.
    json info;
    info["decoders"] = json::array();
    for (auto &kv : st.di_stack) {
        struct srd_decoder *d = kv.first->decoder;
        json jd;
        jd["stack_id"] = kv.second;
        jd["id"] = d->id ? d->id : "";
        jd["rows"] = json::array();
        int rid = 0;
        for (GSList *r = d->annotation_rows; r; r = r->next, rid++) {
            auto *row = (struct srd_decoder_annotation_row *)r->data;
            json jr;
            jr["row_id"] = rid;
            jr["name"] = row->desc ? row->desc : (row->id ? row->id : "");
            jr["classes"] = json::array();
            for (GSList *cc = row->ann_classes; cc; cc = cc->next)
                jr["classes"].push_back({{"index", GPOINTER_TO_INT(cc->data)}});
            jd["rows"].push_back(jr);
        }
        info["decoders"].push_back(jd);
    }
    son_send_json(fd, SON_MSG_DECODER_INFO, info.dump());
}

} // namespace

// ---- post-hoc decode session (re-decode captured data) ---------------------
void *decode_begin(int fd, uint64_t samplerate, const std::vector<DecoderCfg> &decoders)
{
    CapState *st = new CapState();
    st->fd = fd;
    st->samplerate = samplerate;
    CaptureCfg cfg;
    cfg.samplerate = samplerate;
    cfg.decoders = decoders;
    setup_decoders(fd, cfg, *st);
    if (!st->srd_sess) {
        delete st;
        return nullptr;
    }
    return st;
}

void decode_feed(void *state, uint64_t start_sample, const uint8_t *data,
                 uint64_t len, int unitsize)
{
    CapState *st = (CapState *)state;
    if (!st || !st->srd_sess || unitsize <= 0)
        return;
    uint64_t cnt = len / (uint64_t)unitsize;
    srd_session_send(st->srd_sess, start_sample, start_sample + cnt, data, len,
                     (uint64_t)unitsize);
    flush_anns(st);
}

void decode_end(void *state, int fd)
{
    CapState *st = (CapState *)state;
    if (st) {
        flush_anns(st);
        if (st->srd_sess)
            srd_session_destroy(st->srd_sess);
        delete st;
    }
    son_send_json(fd, SON_MSG_DECODE_END, "{}");
}

void run_capture(int fd, const CaptureCfg &cfg)
{
    auto fail = [&](const char *msg) {
        json e = {{"reason", "error"}, {"message", msg}};
        son_send_json(fd, SON_MSG_CAPTURE_END, e.dump());
    };

    // Mark the capture live for the whole scope; cleared on every return path so
    // the server's reaper can tell a finished capture from a truly stuck one.
    g_capture_running = true;
    struct RunGuard { ~RunGuard() { g_capture_running = false; } } run_guard;

    DeviceEntry *dev = find_device(cfg.device_id);
    if (!dev) { fail("unknown device_id"); return; }
    struct sr_dev_inst *sdi = dev->sdi;
    if (sr_dev_open(sdi) != SR_OK) { fail("sr_dev_open failed"); return; }

    // Enable requested channels (all if none specified) and compute metadata.
    std::set<int> want(cfg.channels.begin(), cfg.channels.end());
    int max_logic_idx = -1;
    json meta_channels = json::array();
    for (GSList *c = sr_dev_inst_channels_get(sdi); c; c = c->next) {
        struct sr_channel *ch = (struct sr_channel *)c->data;
        gboolean en = (cfg.channels.empty() || want.count(ch->index)) ? TRUE : FALSE;
        sr_dev_channel_enable(ch, en);
        if (ch->type == SR_CHANNEL_LOGIC && ch->index > max_logic_idx)
            max_logic_idx = ch->index;
        if (en) {
            json jc = {{"index", ch->index},
                       {"name", ch->name ? ch->name : ""},
                       {"type", ch->type == SR_CHANNEL_LOGIC ? "logic" : "analog"}};
            if (ch->type == SR_CHANNEL_LOGIC)
                jc["bit"] = ch->index; // bit position in the logic sample word
            meta_channels.push_back(jc);
        }
    }
    int unitsize = max_logic_idx >= 0 ? (max_logic_idx / 8 + 1) : 1;

    sr_config_set(sdi, nullptr, SR_CONF_SAMPLERATE, g_variant_new_uint64(cfg.samplerate));
    if (cfg.mode != "continuous")
        sr_config_set(sdi, nullptr, SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(cfg.limit_samples));
    // Pre-trigger portion of the buffer (percent). Best-effort: not all drivers
    // support it; a failure just means capture starts at the trigger.
    if (cfg.capture_ratio > 0 && !cfg.triggers.empty()) {
        if (sr_config_set(sdi, nullptr, SR_CONF_CAPTURE_RATIO,
                          g_variant_new_uint64((uint64_t)cfg.capture_ratio)) != SR_OK)
            fprintf(stderr, "[cap] device does not support capture_ratio\n");
    }

    struct sr_session *session = nullptr;
    if (sr_session_new(g_ctx, &session) != SR_OK) { sr_dev_close(sdi); fail("sr_session_new failed"); return; }
    sr_session_dev_add(session, sdi);

    CapState st;
    st.fd = fd;
    st.samplerate = cfg.samplerate;
    st.unitsize = unitsize;
    sr_session_datafeed_callback_add(session, datafeed_cb, &st);

    if (!cfg.triggers.empty()) {
        struct sr_trigger *trig = sr_trigger_new("son");
        struct sr_trigger_stage *stage = sr_trigger_stage_add(trig);
        for (auto &t : cfg.triggers) {
            struct sr_channel *ch = channel_by_index(sdi, t.first);
            int match = trigger_match_from_str(t.second);
            if (ch && match)
                sr_trigger_match_add(stage, ch, match, 0.0f);
        }
        sr_session_trigger_set(session, trig);
    }

    // SESSION_META must precede any data so the client can size its stores.
    json meta;
    meta["samplerate"] = cfg.samplerate;
    meta["unitsize"] = unitsize;
    meta["total_samples"] = (cfg.mode == "continuous") ? 0 : cfg.limit_samples;
    meta["channels"] = meta_channels;
    son_send_json(fd, SON_MSG_SESSION_META, meta.dump());

    if (!cfg.decoders.empty())
        setup_decoders(fd, cfg, st);

    g_active_session = session;
    g_first_data = false;
    g_watchdog_fired = false;

    // Watchdog: force-stop a capture whose DATA phase runs far beyond its
    // expected duration (a hung device). The armed phase — waiting for a
    // trigger — is intentionally NOT bounded: waiting minutes/hours for a rare
    // event is the whole point of a triggered capture (the user can STOP).
    // Continuous mode has no cap either. Joined before session teardown.
    std::thread watchdog;
    {
        double expected = cfg.samplerate > 0 ? (double)cfg.limit_samples / cfg.samplerate : 0.0;
        double cap_s = (cfg.mode == "continuous") ? 0.0 : std::max(30.0, expected * 5.0 + 10.0);
        if (cap_s > 0.0) {
            watchdog = std::thread([session, cap_s]() {
                // armed: wait indefinitely for the first sample
                while (g_active_session.load() == session && !g_first_data.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                auto t0 = std::chrono::steady_clock::now();
                while (g_active_session.load() == session) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    double el = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - t0).count();
                    if (el > cap_s && g_active_session.load() == session) {
                        fprintf(stderr, "[watchdog] data phase exceeded %.1fs; stopping\n", cap_s);
                        g_watchdog_fired = true;
                        sr_session_stop(session);
                        return;
                    }
                }
            });
        }
    }

    int start_rc = sr_session_start(session);
    if (start_rc == SR_OK)
        sr_session_run(session);
    g_active_session = nullptr;
    if (watchdog.joinable())
        watchdog.join();

    if (st.srd_sess)
        flush_anns(&st);

    const char *reason = start_rc != SR_OK       ? "error"
                         : g_watchdog_fired.load() ? "timeout"
                         : g_stop_requested.load() ? "stopped"
                                                   : "complete";
    const char *msg = start_rc != SR_OK ? "sr_session_start failed"
                      : g_watchdog_fired.load()
                          ? "watchdog: device stopped delivering data in time"
                          : "";
    json endj = {{"reason", reason}, {"message", msg}};
    son_send_json(fd, SON_MSG_CAPTURE_END, endj.dump());

    if (st.srd_sess)
        srd_session_destroy(st.srd_sess);
    sr_session_destroy(session);
    sr_dev_close(sdi);
}
