#include "model.h"

namespace son {

static DecOption::Kind classify_option(const json &opt) {
    if (opt.contains("values") && opt["values"].is_array() && !opt["values"].empty())
        return DecOption::ENUM;
    if (opt.contains("default") && opt["default"].is_number())
        return DecOption::INT;
    return DecOption::STR;
}

bool parse_scan_result(const std::string &js, std::vector<DeviceInfo> &out,
                       std::string &err) {
    out.clear();
    try {
        json j = json::parse(js);
        for (auto &d : j.at("devices")) {
            DeviceInfo dev;
            dev.id = d.value("id", -1);
            dev.driver = d.value("driver", "");
            dev.vendor = d.value("vendor", "");
            dev.model = d.value("model", "");
            dev.conn = d.value("conn", "");
            dev.triggers = d.value("triggers", false);
            if (d.contains("samplerates"))
                for (auto &r : d["samplerates"]) dev.samplerates.push_back(r.get<uint64_t>());
            if (d.contains("channels")) {
                for (auto &c : d["channels"]) {
                    ChannelInfo ci;
                    ci.index = c.value("index", 0);
                    ci.name = c.value("name", "");
                    ci.type = c.value("type", "logic");
                    dev.channels.push_back(ci);
                }
            }
            out.push_back(std::move(dev));
        }
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

bool parse_decoders_list(const std::string &js, std::vector<DecoderMeta> &out,
                         std::string &err) {
    out.clear();
    try {
        json j = json::parse(js);
        for (auto &d : j.at("decoders")) {
            DecoderMeta m;
            m.id = d.value("id", "");
            m.name = d.value("name", m.id);
            m.longname = d.value("longname", "");
            if (d.contains("tags"))
                for (auto &t : d["tags"]) m.tags.push_back(t.get<std::string>());
            if (d.contains("channels")) {
                for (auto &c : d["channels"]) {
                    DecChannel dc;
                    dc.id = c.value("id", "");
                    dc.name = c.value("name", dc.id);
                    dc.desc = c.value("desc", "");
                    dc.required = c.value("required", false);
                    m.channels.push_back(dc);
                }
            }
            if (d.contains("options")) {
                for (auto &o : d["options"]) {
                    DecOption op;
                    op.id = o.value("id", "");
                    op.desc = o.value("desc", op.id);
                    if (o.contains("default")) op.def = o["default"];
                    if (o.contains("values") && o["values"].is_array())
                        for (auto &v : o["values"])
                            op.values.push_back(v.is_string() ? v.get<std::string>() : v.dump());
                    op.kind = classify_option(o);
                    m.options.push_back(op);
                }
            }
            out.push_back(std::move(m));
        }
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

bool parse_session_meta(const std::string &js, SessionMeta &out, std::string &err) {
    try {
        json j = json::parse(js);
        out.samplerate = j.value("samplerate", (uint64_t)0);
        out.unitsize = (uint8_t)j.value("unitsize", 1);
        out.total_samples = j.value("total_samples", (uint64_t)0);
        out.channels.clear();
        if (j.contains("channels")) {
            for (auto &c : j["channels"]) {
                ChannelInfo ci;
                ci.index = c.value("index", 0);
                ci.name = c.value("name", "");
                ci.type = c.value("type", "logic");
                ci.bit = c.value("bit", -1);
                out.channels.push_back(ci);
            }
        }
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

bool parse_decoder_info(const std::string &js, std::vector<DecoderRowInfo> &out,
                        std::string &err) {
    out.clear();
    try {
        json j = json::parse(js);
        for (auto &d : j.at("decoders")) {
            DecoderRowInfo di;
            di.stack_id = d.value("stack_id", 0u);
            di.id = d.value("id", "");
            if (d.contains("rows")) {
                for (auto &r : d["rows"]) {
                    int rid = r.value("row_id", 0);
                    std::string nm = r.value("name", "");
                    di.rows.push_back({rid, nm});
                }
            }
            out.push_back(std::move(di));
        }
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    return true;
}

void init_decoder_defaults(DecoderInstance &inst) {
    inst.opts = json::object();
    for (const auto &op : inst.meta.options) {
        if (!op.def.is_null())
            inst.opts[op.id] = op.def;
        else if (op.kind == DecOption::ENUM && !op.values.empty())
            inst.opts[op.id] = op.values[0];
        else if (op.kind == DecOption::INT)
            inst.opts[op.id] = 0;
        else
            inst.opts[op.id] = "";
    }
    for (const auto &c : inst.meta.channels)
        if (!inst.ch_map.count(c.id)) inst.ch_map[c.id] = -1;
}

std::string build_config(int device_id, uint64_t samplerate,
                         const std::vector<int> &channels, const std::string &mode,
                         uint64_t limit_samples, int capture_ratio,
                         const std::vector<std::pair<int, std::string>> &triggers,
                         const std::vector<DecoderInstance> &decoders) {
    json j;
    j["device_id"] = device_id;
    j["samplerate"] = samplerate;
    j["channels"] = channels;
    j["mode"] = mode;
    if (mode == "triggered") j["limit_samples"] = limit_samples;
    if (capture_ratio > 0) j["capture_ratio"] = capture_ratio;

    if (!triggers.empty()) {
        json ta = json::array();
        for (auto &t : triggers) {
            if (t.second == "none") continue;
            ta.push_back({{"channel", t.first}, {"match", t.second}});
        }
        if (!ta.empty()) j["triggers"] = ta;
    }

    if (!decoders.empty()) j["decoders"] = decoders_to_json(decoders);
    return j.dump();
}

json decoders_to_json(const std::vector<DecoderInstance> &decoders) {
    json da = json::array();
    for (const auto &d : decoders) {
        json dj;
        dj["id"] = d.meta.id;
        json ch = json::object();
        for (const auto &kv : d.ch_map)
            if (kv.second >= 0) ch[kv.first] = kv.second;
        dj["channels"] = ch;
        dj["options"] = d.opts;
        da.push_back(dj);
    }
    return da;
}

}  // namespace son
