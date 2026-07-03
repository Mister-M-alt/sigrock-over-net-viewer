// Waveform canvas: digital traces (transition pyramid), analog envelopes,
// annotation rows, time grid and cursors, all via ImDrawList.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app.h"
#include "imgui.h"

namespace son {

static const float LABEL_W = 130.0f;
static const float ROW_H = 46.0f;
static const float ANN_H = 20.0f;

static ImU32 class_color(int c) {
    ImColor col = ImColor::HSV(std::fmod(c * 0.13f + 0.05f, 1.0f), 0.55f, 0.85f, 1.0f);
    return (ImU32)col;
}

static ImU32 marker_color(size_t i) {
    ImColor col = ImColor::HSV(std::fmod(i * 0.19f + 0.11f, 1.0f), 0.75f, 1.0f, 1.0f);
    return (ImU32)col;
}

// Per-channel trace color (golden-angle palette): channels get a visual
// identity instead of uniform green/blue.
static ImU32 chan_color(int index, bool analog) {
    float h = std::fmod(0.38f + index * 0.618034f, 1.0f);
    ImColor col = analog ? ImColor::HSV(h, 0.45f, 0.95f) : ImColor::HSV(h, 0.62f, 0.92f);
    return (ImU32)col;
}

void App::view_fit(float canvas_w) {
    uint64_t total = logic().count(), fl = logic().first_live();
    if (total <= fl) return;
    view_start_ = (double)fl;
    spp_ = std::max(1.0 / 64.0, (double)(total - fl) / std::max(1.0f, canvas_w));
    view_init_ = true;
}

void App::view_center_on(double sample) {
    view_start_ = sample - (double)last_canvas_w_ * spp_ * 0.5;
    follow_ = false;
    user_view_touched_ = true;
    view_init_ = true;
}

// Nearest edge of `bit` within +-window samples of s (for marker snapping).
bool App::nearest_edge(int bit, double s, double window, double &edge) const {
    if (window < 1.0) window = 1.0;
    uint64_t lo = s > window ? (uint64_t)(s - window) : 0;
    uint64_t hi = (uint64_t)(s + window) + 1;
    std::vector<Edge> edges;
    uint8_t init;
    logic().walk(bit, lo, hi, 1.0, init, edges, nullptr);
    bool found = false;
    double best = 0, bestd = 0;
    for (auto &e : edges) {
        double d = std::fabs((double)e.sample - s);
        if (!found || d < bestd) { found = true; best = (double)e.sample; bestd = d; }
    }
    if (found) edge = best;
    return found;
}

static std::string fmt_time(double sec) {
    char b[64];
    double a = std::fabs(sec);
    if (a >= 1.0) std::snprintf(b, sizeof(b), "%.3g s", sec);
    else if (a >= 1e-3) std::snprintf(b, sizeof(b), "%.3g ms", sec * 1e3);
    else if (a >= 1e-6) std::snprintf(b, sizeof(b), "%.3g us", sec * 1e6);
    else std::snprintf(b, sizeof(b), "%.3g ns", sec * 1e9);
    return b;
}

// unit chosen from a scale `sc` (s/ms/us/ns): grid ticks pass the tick step so
// fine zoom shows ns; markers pass the samples/pixel time so labels reach ns.
static void unit_for(double sc, const char *&u, double &div) {
    double a = std::fabs(sc);
    if (a >= 1.0)       { u = "s";  div = 1.0;  }
    else if (a >= 1e-3) { u = "ms"; div = 1e-3; }
    else if (a >= 1e-6) { u = "us"; div = 1e-6; }
    else                { u = "ns"; div = 1e-9; }
}

// Grid tick: unit from the step, enough decimals to resolve it.
static std::string fmt_grid(double value, double step) {
    const char *u; double div;
    unit_for(step, u, div);
    double su = std::fabs(step) / div;
    int dec = (su > 0 && su < 1) ? (int)std::ceil(-std::log10(su)) : 0;
    if (dec > 3) dec = 3;
    char b[80];
    std::snprintf(b, sizeof(b), "%.*f %s", dec, value / div, u);
    return b;
}

void App::clamp_view(float canvas_w) {
    if (spp_ < 1.0 / 64.0) spp_ = 1.0 / 64.0;
    if (spp_ > 1e9) spp_ = 1e9;
    double firstlive = (double)logic().first_live();
    double total = (double)logic().count();
    if (view_total_hint_ > total) total = view_total_hint_;
    // Don't zoom out past ~1.5x the capture: endless mouse-wheel-out recovery
    // is pure frustration.
    if (total > firstlive) {
        double max_spp = (total - firstlive) * 1.5 / std::max(1.0f, canvas_w);
        if (max_spp > 1.0 / 64.0 && spp_ > max_spp) spp_ = max_spp;
    }
    double max_start = total - (double)canvas_w * spp_;
    if (max_start < firstlive) max_start = firstlive;
    if (view_start_ < firstlive) view_start_ = firstlive;
    if (view_start_ > max_start) view_start_ = max_start;
}

void App::draw_time_grid(void *d, float x0, float x1, float y0, float y1) {
    ImDrawList *dl = (ImDrawList *)d;
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    double sec_per_px = spp_ / sr;
    double raw_step = 90.0 * sec_per_px;
    if (raw_step <= 0) return;
    double mag = std::pow(10.0, std::floor(std::log10(raw_step)));
    double n = raw_step / mag;
    double nice = n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10;
    double step = nice * mag;  // seconds

    double left_sec = view_start_ / sr;
    double t = std::ceil(left_sec / step) * step;
    ImU32 grid = IM_COL32(60, 60, 70, 255);
    ImU32 txt = IM_COL32(150, 150, 160, 255);
    for (int guard = 0; guard < 10000; ++guard, t += step) {
        double sample = t * sr;
        float gx = (float)x_of(sample, x0);
        if (gx > x1) break;
        if (gx < x0) continue;
        dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), grid, 1.0f);
        dl->AddText(ImVec2(gx + 2, y0), txt, fmt_grid(t, step).c_str());
    }
}

void App::draw_logic_row(void *d, const ChannelInfo &ch, float x0, float x1,
                         float y_top, float y_bot) {
    ImDrawList *dl = (ImDrawList *)d;
    int bit = ch.bit;
    if (bit < 0) return;
    float yhi = y_top + 3, ylo = y_bot - 3;
    auto yv = [&](uint8_t v) { return v ? yhi : ylo; };

    double lo = std::max(view_start_, (double)logic().first_live());
    double right = view_start_ + (double)(x1 - x0) * spp_;
    double hi = std::min(right, (double)logic().count());
    if (hi <= lo) return;

    std::vector<Edge> edges;
    uint8_t init = 0;
    logic().walk(bit, (uint64_t)lo, (uint64_t)std::ceil(hi), spp_, init, edges, nullptr);

    ImU32 col = chan_color(ch.index, false);
    std::vector<ImVec2> pts;
    pts.reserve(edges.size() * 4 + 4);
    uint8_t cur = init;
    pts.push_back(ImVec2((float)x_of(lo, x0), yv(cur)));
    size_t i = 0;
    while (i < edges.size()) {
        float ex = (float)x_of((double)edges[i].sample, x0);
        float bucket = std::floor(ex);
        uint8_t endval = cur;
        size_t j = i;
        while (j < edges.size() &&
               std::floor((float)x_of((double)edges[j].sample, x0)) == bucket) {
            endval = edges[j].value;
            ++j;
        }
        pts.push_back(ImVec2(ex, yv(cur)));   // horizontal run up to this pixel
        pts.push_back(ImVec2(ex, yhi));       // full-height activity in this pixel
        pts.push_back(ImVec2(ex, ylo));
        pts.push_back(ImVec2(ex, yv(endval)));
        cur = endval;
        i = j;
    }
    pts.push_back(ImVec2((float)x_of(hi, x0), yv(cur)));
    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.5f);

    for (uint64_t g : logic().gaps()) {
        if ((double)g > lo && (double)g < hi) {
            float gx = (float)x_of((double)g, x0);
            dl->AddLine(ImVec2(gx, y_top), ImVec2(gx, y_bot), IM_COL32(210, 70, 70, 160), 1.5f);
        }
    }
}

void App::draw_analog_row(void *d, const ChannelInfo &ch, float x0, float x1,
                          float y_top, float y_bot) {
    ImDrawList *dl = (ImDrawList *)d;
    AnalogStore *as = analog_get((uint32_t)ch.index);
    if (!as) return;
    double lo = std::max(view_start_, 0.0);
    double right = view_start_ + (double)(x1 - x0) * spp_;
    double hi = std::min(right, (double)as->count());
    if (hi <= lo) return;

    struct Col { float x, mn, mx; };
    std::vector<Col> cols;
    float gmn = 1e30f, gmx = -1e30f;
    int px0 = (int)std::floor(x0), px1 = (int)std::ceil(x1);
    for (int px = px0; px < px1; ++px) {
        double s0 = sample_of((double)px, x0);
        double s1 = sample_of((double)px + 1.0, x0);
        if (s1 <= lo || s0 >= hi) continue;
        float mn, mx;
        if (spp_ >= 16.0) {
            if (!as->envelope((uint64_t)std::max(s0, lo), spp_, mn, mx)) continue;
        } else {
            uint64_t a = (uint64_t)std::max(s0, lo);
            uint64_t b = (uint64_t)std::min(s1, hi);
            if (b <= a) b = a + 1;
            mn = 1e30f; mx = -1e30f;
            for (uint64_t s = a; s < b; ++s) {
                float v = as->value(s);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
        cols.push_back({(float)px, mn, mx});
        gmn = std::min(gmn, mn);
        gmx = std::max(gmx, mx);
    }
    if (cols.empty() || gmx <= gmn) { gmn -= 1; gmx += 1; }
    float h = (y_bot - 3) - (y_top + 3);
    auto ymap = [&](float v) { return (y_bot - 3) - (v - gmn) / (gmx - gmn) * h; };
    ImU32 col = chan_color(ch.index, true);
    for (size_t k = 0; k < cols.size(); ++k) {
        dl->AddLine(ImVec2(cols[k].x, ymap(cols[k].mn)), ImVec2(cols[k].x, ymap(cols[k].mx)), col, 1.0f);
        if (k > 0)
            dl->AddLine(ImVec2(cols[k - 1].x, ymap(cols[k - 1].mx)),
                        ImVec2(cols[k].x, ymap(cols[k].mn)), col, 1.0f);
    }
    // y-scale: the visible min/max of this row (rescales with the view)
    char yb[48];
    std::snprintf(yb, sizeof(yb), "%.3g", gmx);
    dl->AddText(ImVec2(x0 + 3, y_top + 1), IM_COL32(170, 170, 185, 200), yb);
    std::snprintf(yb, sizeof(yb), "%.3g", gmn);
    dl->AddText(ImVec2(x0 + 3, y_bot - 15), IM_COL32(170, 170, 185, 200), yb);
}

void App::draw_ann_rows(void *d, float x0, float x1, float &y) {
    ImDrawList *dl = (ImDrawList *)d;
    double lo = view_start_;
    double right = view_start_ + (double)(x1 - x0) * spp_;
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    ImU32 txtc = IM_COL32(15, 15, 15, 255);
    ImU32 labelc = IM_COL32(210, 210, 220, 255);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    for (auto &dr : decoder_rows_) {
        for (auto &row : dr.rows) {
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ANN_H - 2), IM_COL32(28, 28, 34, 255));
            std::string lab = dr.id + "/" + row.second;
            dl->AddText(ImVec2(x0 - LABEL_W + 2, y + 2), labelc, lab.c_str());
            // visit() draws in place: no per-frame copy of the whole row, and
            // the binary search culls to the visible span. (A4 perf fix.)
            anns().visit(dr.stack_id, (uint16_t)row.first, (uint64_t)std::max(0.0, lo),
                        (uint64_t)std::max(0.0, right), [&](const Annotation &an) {
                float ax = (float)x_of((double)an.start, x0);
                float bx = (float)x_of((double)an.end, x0);
                if (bx - ax < 2) bx = ax + 2;
                ImU32 c = class_color(an.ann_class);
                dl->AddRectFilled(ImVec2(ax, y), ImVec2(bx, y + ANN_H - 2), c, 2.0f);
                dl->AddRect(ImVec2(ax, y), ImVec2(bx, y + ANN_H - 2), IM_COL32(0, 0, 0, 120), 2.0f);
                float w = bx - ax - 4;
                const std::string *chosen = nullptr;  // longest that fits (texts longest-first)
                for (auto &t : an.texts) {
                    if (ImGui::CalcTextSize(t.c_str()).x <= w) { chosen = &t; break; }
                }
                if (!chosen && !an.texts.empty()) chosen = &an.texts.back();
                if (chosen) {
                    dl->PushClipRect(ImVec2(ax, y), ImVec2(bx, y + ANN_H - 2), true);
                    dl->AddText(ImVec2(ax + 2, y + 2), txtc, chosen->c_str());
                    dl->PopClipRect();
                }
                // hover: full text + timing; double-click: zoom to it
                if (mouse.x >= ax && mouse.x <= bx && mouse.y >= y && mouse.y <= y + ANN_H - 2) {
                    ImGui::SetTooltip("%s\n%s .. %s  (%s)",
                                      an.texts.empty() ? "" : an.texts.front().c_str(),
                                      fmt_time((double)an.start / sr).c_str(),
                                      fmt_time((double)an.end / sr).c_str(),
                                      fmt_time((double)(an.end - an.start) / sr).c_str());
                    if (dbl) {
                        double span = std::max(1.0, (double)(an.end - an.start));
                        spp_ = span * 2.0 / std::max(1.0f, (float)(x1 - x0));
                        view_start_ = (double)an.start - span * 0.5;
                        follow_ = false;
                        user_view_touched_ = true;
                    }
                }
            });
            y += ANN_H;
        }
    }
}

void App::draw_markers(void *d, float x0, float x1, float y0, float y1) {
    ImDrawList *dl = (ImDrawList *)d;
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;
    // draw text over an opaque box so labels stay readable over signals/grid.
    auto text_bg = [&](float x, float y, ImU32 col, const char *s) {
        ImVec2 sz = ImGui::CalcTextSize(s);
        dl->AddRectFilled(ImVec2(x - 2, y - 1), ImVec2(x + sz.x + 3, y + sz.y + 1),
                          IM_COL32(8, 8, 12, 215));
        dl->AddText(ImVec2(x, y), col, s);
    };
    for (size_t i = 0; i < markers_.size(); ++i) {
        float gx = (float)x_of(markers_[i].sample, x0);
        if (gx < x0 || gx > x1) continue;
        ImU32 c = marker_color(i);
        dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), c, 1.5f);
        dl->AddTriangleFilled(ImVec2(gx - 4, y0), ImVec2(gx + 4, y0), ImVec2(gx, y0 + 6), c);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%s %s", markers_[i].name.c_str(),
                      fmt_grid(markers_[i].sample / sr, spp_ / sr).c_str());
        // below the grid-label row, staggered so adjacent markers don't overlap
        text_bg(gx + 5, y0 + 18.0f + (float)(i % 4) * 15.0f, c, lbl);
    }
    // Δ of the last two markers, on its own line above the status overlay.
    if (markers_.size() >= 2) {
        double a = markers_[markers_.size() - 2].sample;
        double b = markers_[markers_.size() - 1].sample;
        double dt = std::fabs(b - a) / sr;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "d(%s,%s)=%s  f=%.4g Hz",
                      markers_[markers_.size() - 2].name.c_str(),
                      markers_[markers_.size() - 1].name.c_str(),
                      fmt_time(dt).c_str(), dt > 0 ? 1.0 / dt : 0.0);
        text_bg(x0 + 6, y1 - 34.0f, IM_COL32(255, 255, 255, 255), buf);
    }
}

void App::draw_canvas_panel() {
    ImGui::SetNextWindowPos(ImVec2(412, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(980, 718), ImGuiCond_FirstUseEver);
    ImGui::Begin("Waveform");
    SessionMeta meta = meta_copy();
    render_sr_ = (double)meta.samplerate;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 50 || avail.y < 30) { ImGui::End(); return; }

    float x0 = p0.x + LABEL_W;
    float x1 = p0.x + avail.x;
    float grid_top = p0.y;
    float rows_top = p0.y + 20.0f;
    float y_bottom = p0.y + avail.y;
    float canvas_w = std::max(1.0f, x1 - x0);

    last_canvas_w_ = canvas_w;
    view_total_hint_ = (capturing_ || meta_pending_.load()) ? (double)meta.total_samples : 0.0;
    ImGui::InvisibleButton("canvas", ImVec2(avail.x, avail.y));
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // view init / follow
    if (need_view_reset_.exchange(false)) view_init_ = false;
    uint64_t total = logic().count();
    if (!view_init_ && total > 0) view_fit(canvas_w);

    // display order + auto row height: shrink rows (down to 18 px) so all
    // channels and annotation lanes fit instead of silently vanishing.
    std::vector<const ChannelInfo *> ord = ordered_channels(meta);
    int lanes = 0;
    for (auto &dr : decoder_rows_) lanes += (int)dr.rows.size();
    float row_h = ROW_H;
    if (!ord.empty()) {
        float free_h = (y_bottom - rows_top) - lanes * ANN_H - 20.0f;
        row_h = free_h / (float)ord.size();
        if (row_h > ROW_H) row_h = ROW_H;
        if (row_h < 18.0f) row_h = 18.0f;
    }

    // the channel row under a y position (for snapping / hover readout)
    auto chan_at = [&](float ypos) -> const ChannelInfo * {
        if (ypos < rows_top) return nullptr;
        int idx = (int)((ypos - rows_top) / row_h);
        if (idx < 0 || idx >= (int)ord.size()) return nullptr;
        return ord[idx];
    };
    // marker placement: snap to the nearest signal edge of the row under the
    // mouse (within ~10 px) unless Alt is held. Pixel-accurate measurements.
    auto place = [&](double raw, float ypos) -> double {
        if (io.KeyAlt) return raw;
        const ChannelInfo *ch = chan_at(ypos);
        if (!ch || ch->type != "logic" || ch->bit < 0) return raw;
        double edge;
        if (nearest_edge(ch->bit, raw, 10.0 * spp_, edge)) return edge;
        return raw;
    };

    // keyboard navigation (window focused, not typing)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        double pan = 60.0 * spp_ * (io.KeyShift ? 4.0 : 1.0);
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { view_start_ -= pan; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { view_start_ += pan; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) { view_start_ = (double)logic().first_live(); moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) { view_start_ = (double)total - canvas_w * spp_; moved = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) { view_fit(canvas_w); moved = true; }
        double zoom = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) zoom = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) zoom = -1;
        if (zoom != 0) {
            double c = view_start_ + canvas_w * spp_ * 0.5;
            spp_ *= std::pow(0.7, zoom);
            view_start_ = c - canvas_w * spp_ * 0.5;
            moved = true;
        }
        if (moved) {
            follow_ = false;
            user_view_touched_ = true;
        }
    }

    // input
    if (hovered) {
        if (io.MouseWheel != 0.0f) {
            double m = sample_of((double)io.MousePos.x, x0);
            spp_ *= std::pow(0.82, io.MouseWheel);
            if (spp_ < 1.0 / 64.0) spp_ = 1.0 / 64.0;
            view_start_ = m - (double)(io.MousePos.x - x0) * spp_;
            user_view_touched_ = true;
        }
        float mx = io.MousePos.x;
        // grab an existing marker to drag it, else pan
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag_marker_ = -1;
            for (size_t i = 0; i < markers_.size(); ++i)
                if (std::fabs((float)x_of(markers_[i].sample, x0) - mx) <= 4.0f) {
                    drag_marker_ = (int)i; break;
                }
        }
        // right-click near a marker deletes it
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            for (size_t i = 0; i < markers_.size(); ++i)
                if (std::fabs((float)x_of(markers_[i].sample, x0) - mx) <= 4.0f) {
                    markers_.erase(markers_.begin() + i);
                    break;
                }
        }
        bool on_marker = drag_marker_ >= 0;
        if (on_marker && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (drag_marker_ < (int)markers_.size())
                markers_[drag_marker_].sample =
                    place(sample_of((double)mx, x0), io.MousePos.y);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                   (!on_marker && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            view_start_ -= (double)io.MouseDelta.x * spp_;
            follow_ = false;
            user_view_touched_ = true;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (drag_marker_ < 0 && std::fabs(dd.x) < 3 && std::fabs(dd.y) < 3) {
                Marker m;
                m.sample = place(sample_of((double)mx, x0), io.MousePos.y);
                m.name = "m" + std::to_string(++marker_seq_);
                markers_.push_back(m);
            }
            drag_marker_ = -1;
        }
    }

    if (follow_ && capturing_ && total > 0)
        view_start_ = (double)total - (double)canvas_w * spp_;
    clamp_view(canvas_w);

    // background + grid
    dl->AddRectFilled(p0, ImVec2(x1, y_bottom), IM_COL32(18, 18, 22, 255));
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(x0, y_bottom), IM_COL32(30, 30, 36, 255));
    dl->PushClipRect(ImVec2(x0, grid_top), ImVec2(x1, y_bottom), true);
    draw_time_grid(dl, x0, x1, grid_top, y_bottom);
    dl->PopClipRect();

    // rows (in user display order, auto-fitted height)
    float y = rows_top;
    ImU32 labelc = IM_COL32(220, 220, 230, 255);
    dl->PushClipRect(ImVec2(x0, rows_top), ImVec2(x1, y_bottom), true);
    for (const ChannelInfo *ch : ord) {
        float rt = y, rb = y + row_h;
        if (rb > y_bottom) break;
        if (ch->type == "logic") draw_logic_row(dl, *ch, x0, x1, rt, rb);
        else draw_analog_row(dl, *ch, x0, x1, rt, rb);
        y += row_h;
    }
    // annotation rows below signals
    draw_ann_rows(dl, x0, x1, y);
    dl->PopClipRect();

    // labels (outside clip)
    y = rows_top;
    for (const ChannelInfo *ch : ord) {
        if (y + row_h > y_bottom) break;
        std::string nm = chan_name(ch->index, ch->name);
        dl->AddText(ImVec2(p0.x + 4, y + row_h * 0.5f - 7), labelc, nm.c_str());
        dl->AddLine(ImVec2(x0, y + row_h), ImVec2(x1, y + row_h), IM_COL32(45, 45, 52, 255), 1.0f);
        y += row_h;
    }

    // trigger-fired marker ("T"): where the event actually happened
    if (has_trigger_.load()) {
        float tx = (float)x_of((double)trigger_sample_.load(), x0);
        if (tx >= x0 && tx <= x1) {
            ImU32 tc = IM_COL32(255, 120, 60, 255);
            dl->AddLine(ImVec2(tx, grid_top), ImVec2(tx, y_bottom), tc, 2.0f);
            dl->AddText(ImVec2(tx + 3, grid_top + 2), tc, "T");
        }
    }

    // markers on top
    draw_markers(dl, x0, x1, grid_top, y_bottom);

    // hover readout: time + value of the channel under the cursor
    if (hovered && io.MousePos.x >= x0) {
        double s = sample_of((double)io.MousePos.x, x0);
        double sr_h = render_sr_ > 0 ? render_sr_ : 1.0;
        const ChannelInfo *ch = chan_at(io.MousePos.y);
        char hb[160];
        if (ch && s >= 0 && (uint64_t)s < total) {
            if (ch->type == "logic" && ch->bit >= 0) {
                std::snprintf(hb, sizeof(hb), "%s | %s = %d", fmt_time(s / sr_h).c_str(),
                              chan_name(ch->index, ch->name).c_str(),
                              (int)logic().bit(ch->bit, (uint64_t)s));
            } else {
                AnalogStore *as = analog_get((uint32_t)ch->index);
                std::snprintf(hb, sizeof(hb), "%s | %s = %.4g", fmt_time(s / sr_h).c_str(),
                              chan_name(ch->index, ch->name).c_str(),
                              as ? as->value((uint64_t)s) : 0.0f);
            }
        } else {
            std::snprintf(hb, sizeof(hb), "%s", fmt_time(s / sr_h).c_str());
        }
        ImVec2 sz = ImGui::CalcTextSize(hb);
        float hx = io.MousePos.x + 12, hy = io.MousePos.y - 18;
        if (hx + sz.x + 6 > x1) hx = io.MousePos.x - sz.x - 12;
        dl->AddRectFilled(ImVec2(hx - 3, hy - 2), ImVec2(hx + sz.x + 3, hy + sz.y + 2),
                          IM_COL32(8, 8, 12, 220));
        dl->AddText(ImVec2(hx, hy), IM_COL32(235, 235, 245, 255), hb);
    }

    // status overlay — timebase scales with zoom
    char ov[200];
    double sr2 = render_sr_ > 0 ? render_sr_ : 1.0;
    std::snprintf(ov, sizeof(ov), "%.4g samples/px  |  span=%s  |  total=%llu (%s)",
                  spp_, fmt_time(canvas_w * spp_ / sr2).c_str(),
                  (unsigned long long)total, fmt_time((double)total / sr2).c_str());
    dl->AddText(ImVec2(x0 + 6, y_bottom - 16), IM_COL32(180, 180, 190, 220), ov);

    ImGui::End();
}

// ---- oscilloscope view -----------------------------------------------------
static float snap125(float v) {  // round up to the classic 1-2-5 sequence
    if (v <= 0) return 1.0f;
    float mag = std::pow(10.0f, std::floor(std::log10(v)));
    float n = v / mag;
    float s = n <= 1.0f ? 1.0f : n <= 2.0f ? 2.0f : n <= 5.0f ? 5.0f : 10.0f;
    return s * mag;
}

static std::string fmt_volts(float v) {
    char b[32];
    float a = std::fabs(v);
    if (a > 0 && a < 1.0f) std::snprintf(b, sizeof(b), "%g mV", v * 1000.0f);
    else std::snprintf(b, sizeof(b), "%g V", v);
    return b;
}

// Fit a channel: center on the visible mid, ~6 of 8 divisions of swing.
void App::scope_autofit(int chan_index, ScopeChan &sc) {
    AnalogStore *as = analog_get((uint32_t)chan_index);
    if (!as || as->count() == 0) return;
    uint64_t lo = (uint64_t)std::max(view_start_, 0.0);
    uint64_t hi = std::min<uint64_t>(as->count(), (uint64_t)(view_start_ + last_canvas_w_ * spp_));
    if (hi <= lo) { lo = 0; hi = as->count(); }
    uint64_t stride = std::max<uint64_t>(1, (hi - lo) / 4096);
    float mn = 1e30f, mx = -1e30f;
    for (uint64_t s = lo; s < hi; s += stride) {
        float v = as->value(s);
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (mx <= mn) { mn -= 1; mx += 1; }
    sc.voff = (mn + mx) * 0.5f;
    sc.vdiv = snap125((mx - mn) / 6.0f);
    sc.inited = true;
}

void App::draw_scope_panel() {
    ImGui::Begin("Scope");
    SessionMeta meta = meta_copy();
    double sr = render_sr_ > 0 ? render_sr_ : 1.0;

    std::vector<const ChannelInfo *> achs, dchs;
    for (const ChannelInfo *ch : ordered_channels(meta)) {
        if (ch->type == "analog") achs.push_back(ch);
        else if (ch->type == "logic" && ch->bit >= 0) dchs.push_back(ch);
    }
    if (achs.empty() && dchs.empty()) {
        ImGui::TextDisabled("no channels in this capture");
        ImGui::End();
        return;
    }

    // ---- per-channel control strip (the usual scope knobs) ----
    static const float VDIVS[] = {0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f,
                                  0.1f,   0.2f,   0.5f,   1.f,   2.f,   5.f,
                                  10.f,   20.f,   50.f};
    float ctl_h = (achs.empty() || dchs.empty()) ? 34.0f : 62.0f;
    ImGui::BeginChild("scopectl", ImVec2(0, ctl_h), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t ci = 0; ci < achs.size(); ++ci) {
        const ChannelInfo *ch = achs[ci];
        ScopeChan &sc = scope_[ch->index];
        ImGui::PushID(ch->index);
        if (ci) ImGui::SameLine(0, 18);
        ImGui::Checkbox("##v", &sc.show);
        ImGui::SameLine(0, 3);
        ImU32 c = chan_color(ch->index, true);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c), "%s",
                           chan_name(ch->index, ch->name).c_str());
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(76);
        std::string cur = fmt_volts(sc.vdiv) + "/div";
        if (ImGui::BeginCombo("##vdiv", cur.c_str())) {
            for (float v : VDIVS)
                if (ImGui::Selectable((fmt_volts(v) + "/div").c_str(), v == sc.vdiv))
                    sc.vdiv = v;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 3);
        ImGui::SetNextItemWidth(64);
        ImGui::DragFloat("##voff", &sc.voff, sc.vdiv * 0.05f, 0, 0, "%.3g V");
        ImGui::SetItemTooltip("Vertical offset (voltage at center)");
        ImGui::SameLine(0, 3);
        if (ImGui::SmallButton("A")) scope_autofit(ch->index, sc);
        ImGui::SetItemTooltip("Auto-fit scale/offset to the visible data");
        ImGui::SameLine(0, 3);
        ImGui::Checkbox("AC", &sc.ac);
        ImGui::SetItemTooltip("AC coupling: subtract the visible-window mean");
        ImGui::PopID();
    }
    // digital channel knobs (MSO style): position + size, in divisions
    for (size_t ci = 0; ci < dchs.size(); ++ci) {
        const ChannelInfo *ch = dchs[ci];
        ScopeDig &sd = scope_d_[ch->index];
        if (!sd.inited) {  // stack from the top of the graticule downward
            sd.pos = 3.6f - 0.42f * (float)ci;
            sd.inited = true;
        }
        ImGui::PushID(1000 + ch->index);
        if (ci) ImGui::SameLine(0, 14);
        ImGui::Checkbox("##v", &sd.show);
        ImGui::SameLine(0, 3);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(chan_color(ch->index, false)), "%s",
                           chan_name(ch->index, ch->name).c_str());
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(56);
        ImGui::DragFloat("##pos", &sd.pos, 0.05f, -4.0f, 4.0f, "%.2f d");
        ImGui::SetItemTooltip("Vertical position (divisions above center)");
        ImGui::SameLine(0, 3);
        ImGui::SetNextItemWidth(48);
        ImGui::DragFloat("##sz", &sd.size, 0.02f, 0.1f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("Trace height (divisions)");
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- graticule + traces, time-locked to the waveform view ----
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60 + LABEL_W || avail.y < 40) { ImGui::End(); return; }
    // Same left gutter as the waveform view so time columns line up vertically.
    float x0 = p0.x + LABEL_W, x1 = p0.x + avail.x;
    float y0 = p0.y, y1 = p0.y + avail.y;
    float w = x1 - x0;

    ImGui::InvisibleButton("scopecanvas", avail);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // trigger-position cursor (the SETTING for the next acquisition): a scope-
    // style flag at the top edge, draggable to adjust the pre-trigger %.
    bool trig_ui = false;
    uint64_t trig_fl = logic().first_live();
    // Map the pre-trigger % onto the record on screen: the delivered samples,
    // capped by the configured acquisition length (devices can under-deliver).
    uint64_t trig_have = logic().count() > trig_fl ? logic().count() - trig_fl : 0;
    uint64_t trig_tot = trig_have;
    if (meta.total_samples && meta.total_samples < trig_tot) trig_tot = meta.total_samples;
    if (mode_idx_ == 0 && trig_tot > 0 && sel_dev_ >= 0 && sel_dev_ < (int)devices_.size())
        for (auto &ch : devices_[sel_dev_].channels)
            if (ch.enabled && ch.trigger != "none" && !ch.trigger.empty()) trig_ui = true;
    auto trig_x = [&]() {
        return (float)x_of((double)trig_fl + capture_ratio_ / 100.0 * (double)trig_tot, x0);
    };

    // shared pan/zoom (same time axis as the waveform view)
    if (hovered || scope_trig_drag_) {
        float mx = io.MousePos.x;
        if (trig_ui && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            std::fabs(mx - trig_x()) <= 8.0f && io.MousePos.y <= y0 + 16.0f)
            scope_trig_drag_ = true;
        if (scope_trig_drag_ && trig_tot > 0) {
            double s = sample_of((double)mx, x0);
            int r = (int)std::lround((s - (double)trig_fl) / (double)trig_tot * 100.0);
            capture_ratio_ = std::max(0, std::min(99, r));
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) scope_trig_drag_ = false;
        }
    }
    if (hovered && !scope_trig_drag_) {
        if (io.MouseWheel != 0.0f) {
            double m = sample_of((double)io.MousePos.x, x0);
            spp_ *= std::pow(0.82, io.MouseWheel);
            if (spp_ < 1.0 / 64.0) spp_ = 1.0 / 64.0;
            view_start_ = m - (double)(io.MousePos.x - x0) * spp_;
            follow_ = false;
            user_view_touched_ = true;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            view_start_ -= (double)io.MouseDelta.x * spp_;
            follow_ = false;
            user_view_touched_ = true;
        }
    }
    clamp_view(w);

    // background + graticule: 8 vertical divisions, time grid shared with the
    // waveform view so both stay visually aligned.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(12, 13, 17, 255));
    dl->AddRectFilled(p0, ImVec2(x0, y1), IM_COL32(30, 30, 36, 255));  // gutter
    float div_h = avail.y / 8.0f;
    for (int i = 1; i < 8; ++i) {
        ImU32 gc = (i == 4) ? IM_COL32(80, 84, 100, 255) : IM_COL32(42, 45, 56, 255);
        dl->AddLine(ImVec2(x0, y0 + i * div_h), ImVec2(x1, y0 + i * div_h), gc, 1.0f);
    }
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    draw_time_grid(dl, x0, x1, y0, y1);

    // traces
    for (auto *ch : achs) {
        ScopeChan &sc = scope_[ch->index];
        AnalogStore *as = analog_get((uint32_t)ch->index);
        if (!as || as->count() == 0) continue;
        if (!sc.inited) scope_autofit(ch->index, sc);
        if (!sc.show) continue;

        double lo = std::max(view_start_, 0.0);
        double right = view_start_ + (double)w * spp_;
        double hi = std::min(right, (double)as->count());
        if (hi <= lo) continue;

        float mean = 0.0f;
        if (sc.ac) {
            uint64_t stride = std::max<uint64_t>(1, (uint64_t)((hi - lo) / 2048));
            double sum = 0;
            uint64_t n = 0;
            for (uint64_t s = (uint64_t)lo; s < (uint64_t)hi; s += stride) {
                sum += as->value(s);
                ++n;
            }
            if (n) mean = (float)(sum / (double)n);
        }
        float mid = (y0 + y1) * 0.5f;
        auto ymap = [&](float v) {
            float y = mid - ((v - mean) - sc.voff) / sc.vdiv * div_h;
            return std::max(y0 + 1, std::min(y1 - 1, y));
        };
        ImU32 c = chan_color(ch->index, true);

        // ground (0 V) marker on the left edge
        float gy = ymap(0.0f);
        dl->AddTriangleFilled(ImVec2(x0, gy - 4), ImVec2(x0, gy + 4), ImVec2(x0 + 6, gy), c);

        float prev_mx_y = 0;
        bool have_prev = false;
        int px0 = (int)std::floor(x0), px1 = (int)std::ceil(x1);
        for (int px = px0; px < px1; ++px) {
            double s0 = sample_of((double)px, x0);
            double s1 = sample_of((double)px + 1.0, x0);
            if (s1 <= lo || s0 >= hi) continue;
            float mn, mx;
            if (spp_ >= 16.0) {
                if (!as->envelope((uint64_t)std::max(s0, lo), spp_, mn, mx)) continue;
            } else {
                uint64_t a = (uint64_t)std::max(s0, lo);
                uint64_t b = (uint64_t)std::min(s1, hi);
                if (b <= a) b = a + 1;
                mn = 1e30f;
                mx = -1e30f;
                for (uint64_t s = a; s < b; ++s) {
                    float v = as->value(s);
                    mn = std::min(mn, v);
                    mx = std::max(mx, v);
                }
            }
            float ymn = ymap(mn), ymx = ymap(mx);
            dl->AddLine(ImVec2((float)px, ymn), ImVec2((float)px, ymx), c, 1.2f);
            if (have_prev)
                dl->AddLine(ImVec2((float)px - 1, prev_mx_y), ImVec2((float)px, ymn), c, 1.2f);
            prev_mx_y = ymx;
            have_prev = true;
        }
    }

    // digital traces (MSO): edge walk like the time view, placed on the graticule
    for (auto *ch : dchs) {
        ScopeDig &sd = scope_d_[ch->index];
        if (!sd.show) continue;
        float mid = (y0 + y1) * 0.5f;
        float ylo = mid - sd.pos * div_h;
        float yhi = ylo - sd.size * div_h;
        auto yv = [&](uint8_t v) { return v ? yhi : ylo; };

        double lo = std::max(view_start_, (double)logic().first_live());
        double right = view_start_ + (double)w * spp_;
        double hi = std::min(right, (double)logic().count());
        if (hi <= lo) continue;
        std::vector<Edge> edges;
        uint8_t init = 0;
        logic().walk(ch->bit, (uint64_t)lo, (uint64_t)std::ceil(hi), spp_, init, edges, nullptr);

        ImU32 c = chan_color(ch->index, false);
        std::vector<ImVec2> pts;
        pts.reserve(edges.size() * 4 + 4);
        uint8_t cur = init;
        pts.push_back(ImVec2((float)x_of(lo, x0), yv(cur)));
        size_t i = 0;
        while (i < edges.size()) {
            float ex = (float)x_of((double)edges[i].sample, x0);
            float bucket = std::floor(ex);
            uint8_t endval = cur;
            size_t j = i;
            while (j < edges.size() &&
                   std::floor((float)x_of((double)edges[j].sample, x0)) == bucket) {
                endval = edges[j].value;
                ++j;
            }
            pts.push_back(ImVec2(ex, yv(cur)));
            pts.push_back(ImVec2(ex, yhi));
            pts.push_back(ImVec2(ex, ylo));
            pts.push_back(ImVec2(ex, yv(endval)));
            cur = endval;
            i = j;
        }
        pts.push_back(ImVec2((float)x_of(hi, x0), yv(cur)));
        dl->AddPolyline(pts.data(), (int)pts.size(), c, 0, 1.3f);
        // channel tag at the left edge of the trace
        dl->AddText(ImVec2(x0 + 3, (yhi + ylo) * 0.5f - 7), c,
                    chan_name(ch->index, ch->name).c_str());
    }

    // fired-trigger line (where the CURRENT acquisition actually triggered)
    if (has_trigger_.load()) {
        float tx = (float)x_of((double)trigger_sample_.load(), x0);
        if (tx >= x0 && tx <= x1) {
            dl->AddLine(ImVec2(tx, y0), ImVec2(tx, y1), IM_COL32(255, 120, 60, 130), 1.5f);
        }
    }

    // hover readout: time + each visible channel's value at the cursor
    if (hovered && io.MousePos.x >= x0) {
        double s = sample_of((double)io.MousePos.x, x0);
        dl->AddLine(ImVec2(io.MousePos.x, y0), ImVec2(io.MousePos.x, y1),
                    IM_COL32(200, 200, 210, 60), 1.0f);
        std::string hb = fmt_time(s / sr);
        for (auto *ch : achs) {
            ScopeChan &sc = scope_[ch->index];
            AnalogStore *as = analog_get((uint32_t)ch->index);
            if (!sc.show || !as || (uint64_t)s >= as->count() || s < 0) continue;
            hb += "  " + chan_name(ch->index, ch->name) + "=" +
                  fmt_volts(as->value((uint64_t)s));
        }
        for (auto *ch : dchs) {
            ScopeDig &sd = scope_d_[ch->index];
            if (!sd.show || s < 0 || (uint64_t)s >= logic().count()) continue;
            hb += "  " + chan_name(ch->index, ch->name) + "=" +
                  std::to_string((int)logic().bit(ch->bit, (uint64_t)s));
        }
        ImVec2 sz = ImGui::CalcTextSize(hb.c_str());
        float hx = io.MousePos.x + 12, hy = y0 + 4;
        if (hx + sz.x + 6 > x1) hx = io.MousePos.x - sz.x - 12;
        dl->AddRectFilled(ImVec2(hx - 3, hy - 2), ImVec2(hx + sz.x + 3, hy + sz.y + 2),
                          IM_COL32(8, 8, 12, 220));
        dl->AddText(ImVec2(hx, hy), IM_COL32(235, 235, 245, 255), hb.c_str());
    }

    // draggable trigger-position flag (setting for the NEXT acquisition) —
    // drawn last so nothing (traces, hover box) can paint over the handle.
    if (trig_ui) {
        float tx = trig_x();
        if (tx >= x0 - 8 && tx <= x1 + 8) {
            ImU32 tc = IM_COL32(255, 150, 60, 255);
            dl->AddLine(ImVec2(tx, y0), ImVec2(tx, y1), IM_COL32(255, 150, 60, 70), 1.0f);
            dl->AddTriangleFilled(ImVec2(tx - 7, y0), ImVec2(tx + 7, y0), ImVec2(tx, y0 + 11), tc);
            dl->AddText(ImVec2(tx - 3, y0 - 1), IM_COL32(20, 20, 24, 255), "T");
            bool near = hovered && std::fabs(io.MousePos.x - tx) <= 8.0f &&
                        io.MousePos.y <= y0 + 16.0f;
            if (near || scope_trig_drag_)
                ImGui::SetTooltip("trigger position: %d%% pre-trigger\n"
                                  "(drag; applies to the next acquisition)",
                                  capture_ratio_);
        }
    }

    dl->PopClipRect();

    // per-channel scale legend in the left gutter (out of the traces' way)
    float ly = y0 + 4;
    for (auto *ch : achs) {
        ScopeChan &sc = scope_[ch->index];
        if (!sc.show) continue;
        char lb[96];
        std::snprintf(lb, sizeof(lb), "%s %s/div", chan_name(ch->index, ch->name).c_str(),
                      fmt_volts(sc.vdiv).c_str());
        dl->AddText(ImVec2(p0.x + 4, ly), chan_color(ch->index, true), lb);
        char lb2[48];
        std::snprintf(lb2, sizeof(lb2), "  @ %s", fmt_volts(sc.voff).c_str());
        dl->AddText(ImVec2(p0.x + 4, ly + 13), IM_COL32(150, 150, 162, 255), lb2);
        ly += 29;
    }

    ImGui::End();
}

}  // namespace son
