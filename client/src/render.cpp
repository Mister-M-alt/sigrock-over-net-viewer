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
    uint64_t total = logic_.count(), fl = logic_.first_live();
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
    logic_.walk(bit, lo, hi, 1.0, init, edges, nullptr);
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
    double firstlive = (double)logic_.first_live();
    double total = (double)logic_.count();
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

    double lo = std::max(view_start_, (double)logic_.first_live());
    double right = view_start_ + (double)(x1 - x0) * spp_;
    double hi = std::min(right, (double)logic_.count());
    if (hi <= lo) return;

    std::vector<Edge> edges;
    uint8_t init = 0;
    logic_.walk(bit, (uint64_t)lo, (uint64_t)std::ceil(hi), spp_, init, edges, nullptr);

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

    for (uint64_t g : logic_.gaps()) {
        if ((double)g > lo && (double)g < hi) {
            float gx = (float)x_of((double)g, x0);
            dl->AddLine(ImVec2(gx, y_top), ImVec2(gx, y_bot), IM_COL32(210, 70, 70, 160), 1.5f);
        }
    }
}

void App::draw_analog_row(void *d, const ChannelInfo &ch, float x0, float x1,
                          float y_top, float y_bot) {
    ImDrawList *dl = (ImDrawList *)d;
    AnalogStore *as = analog_get((uint32_t)ch.index, false);
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
            anns_.visit(dr.stack_id, (uint16_t)row.first, (uint64_t)std::max(0.0, lo),
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
    ImGui::InvisibleButton("canvas", ImVec2(avail.x, avail.y));
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // view init / follow
    if (need_view_reset_.exchange(false)) view_init_ = false;
    uint64_t total = logic_.count();
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
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) { view_start_ = (double)logic_.first_live(); moved = true; }
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
                              (int)logic_.bit(ch->bit, (uint64_t)s));
            } else {
                AnalogStore *as = analog_get((uint32_t)ch->index, false);
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

}  // namespace son
