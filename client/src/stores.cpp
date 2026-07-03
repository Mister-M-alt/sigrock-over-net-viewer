#include "stores.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace son {

// ======================= LogicStore =======================
LogicStore::LogicStore() {
    for (size_t i = 0; i < SLOTS; ++i) chunks_[i].store(nullptr, std::memory_order_relaxed);
}
LogicStore::~LogicStore() {
    for (size_t i = 0; i < SLOTS; ++i) delete[] chunks_[i].load(std::memory_order_relaxed);
    for (auto *p : pyr_) delete p;
}

void LogicStore::reset(uint8_t unitsize) {
    std::unique_lock<std::shared_mutex> lk(reclaim_mtx_);
    for (size_t i = 0; i < SLOTS; ++i) {
        delete[] chunks_[i].exchange(nullptr, std::memory_order_relaxed);
    }
    for (auto *&p : pyr_) { delete p; p = nullptr; }
    tracked_.clear();
    {
        std::lock_guard<std::mutex> gl(gaps_mtx_);
        gaps_.clear();
    }
    unitsize_ = unitsize ? unitsize : 1;
    written_end_ = 0;
    count_.store(0, std::memory_order_release);
    first_live_.store(0, std::memory_order_release);
}

void LogicStore::set_tracked_bits(const std::vector<int> &bits) {
    tracked_ = bits;
    for (int b : bits) {
        if (b >= 0 && b < 64 && !pyr_[b]) pyr_[b] = new BitPyramid();
    }
}

const BitPyramid *LogicStore::pyramid_for(int bit) const {
    return (bit >= 0 && bit < 64) ? pyr_[bit] : nullptr;
}

uint8_t *LogicStore::ensure_word(uint64_t s) {
    size_t slot = (size_t)(s / SPC);
    if (slot >= SLOTS) return nullptr;  // capacity limit: never write out of bounds
    uint8_t *c = chunks_[slot].load(std::memory_order_relaxed);
    if (!c) {
        c = new uint8_t[(size_t)SPC * unitsize_]();
        chunks_[slot].store(c, std::memory_order_release);
    }
    return c + (size_t)(s % SPC) * unitsize_;
}

const uint8_t *LogicStore::word_read(uint64_t s) const {
    size_t slot = (size_t)(s / SPC);
    if (slot >= SLOTS) return nullptr;
    uint8_t *c = chunks_[slot].load(std::memory_order_acquire);
    if (!c) return nullptr;
    return c + (size_t)(s % SPC) * unitsize_;
}

void LogicStore::append(uint64_t start, uint32_t count, const uint8_t *data,
                        bool discontinuity) {
    if (unitsize_ == 0 || count == 0) return;
    if (!discontinuity && start > written_end_) discontinuity = true;
    if (discontinuity && start > 0) {
        std::lock_guard<std::mutex> gl(gaps_mtx_);
        gaps_.push_back(start);
    }

    auto assemble = [&](const uint8_t *p) -> uint64_t {
        uint64_t w = 0;
        for (int k = 0; k < unitsize_; ++k) w |= (uint64_t)p[k] << (8 * k);
        return w;
    };

    bool has_prev = (start > 0) && !discontinuity && (word_read(start - 1) != nullptr);
    uint64_t prev = 0;
    if (has_prev) prev = assemble(word_read(start - 1));

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t s = start + i;
        uint8_t *dst = ensure_word(s);
        if (!dst) {  // hit the 2^32-sample capacity: drop the rest, stay safe
            full_.store(true, std::memory_order_relaxed);
            count = i;
            break;
        }
        std::memcpy(dst, data + (size_t)i * unitsize_, unitsize_);
        uint64_t w = assemble(dst);
        if (i > 0 || has_prev) {
            uint64_t diff = w ^ prev;
            if (diff) {
                for (int ch : tracked_)
                    if ((diff >> ch) & 1u) pyr_[ch]->set_transition(s);
            }
        }
        prev = w;
    }
    if (count && start + count > written_end_) written_end_ = start + count;
    count_.store(written_end_, std::memory_order_release);
    if (max_samples_) trim();
}

void LogicStore::trim() {
    uint64_t fl = first_live_.load(std::memory_order_relaxed);
    if (written_end_ - fl <= max_samples_) return;
    uint64_t nf = written_end_ - max_samples_;
    std::unique_lock<std::shared_mutex> lk(reclaim_mtx_);
    uint64_t free_upto = nf / SPC;       // chunks strictly below this are dead
    uint64_t start_chunk = fl / SPC;
    for (uint64_t c = start_chunk; c < free_upto && c < SLOTS; ++c)
        delete[] chunks_[c].exchange(nullptr, std::memory_order_relaxed);
    // Reclaim the pyramids and stale gap markers too, or a long continuous
    // session slowly leaks (~count/16 bytes per tracked channel).
    for (int b : tracked_)
        if (pyr_[b]) pyr_[b]->trim(nf);
    {
        std::lock_guard<std::mutex> gl(gaps_mtx_);
        gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(),
                                   [nf](uint64_t g) { return g < nf; }),
                    gaps_.end());
    }
    first_live_.store(nf, std::memory_order_release);
}

std::vector<uint64_t> LogicStore::gaps() const {
    std::lock_guard<std::mutex> gl(gaps_mtx_);
    return gaps_;
}

void LogicStore::walk_block(int ch, int level, uint64_t block, uint64_t vfirst,
                            uint64_t vlast, uint8_t &cur, std::vector<Edge> &out,
                            uint64_t *blocks_scanned) const {
    uint64_t bs = pyr_block_size(level);
    uint64_t bstart = block * bs;
    uint64_t bend = bstart + bs;
    uint64_t lo = std::max(bstart, vfirst);
    uint64_t hi = std::min(bend, vlast);
    if (lo >= hi) return;

    const BitPyramid *p = pyr_[ch];
    uint8_t flag = p ? p->flag(level, block) : 1;  // no pyramid => scan raw
    if (!flag) return;  // constant across block: cur unchanged, nothing to emit

    if (level == 0) {
        if (blocks_scanned) (*blocks_scanned)++;
        for (uint64_t s = lo; s < hi; ++s) {
            uint8_t v = bit(ch, s);
            if (s == vfirst) { cur = v; continue; }  // no edge at left boundary
            uint8_t prev = bit(ch, s - 1);
            if (v != prev) out.push_back({s, v});
            cur = v;
        }
        return;
    }
    uint64_t child_bs = pyr_block_size(level - 1);
    uint64_t c0 = lo / child_bs;
    uint64_t c1 = (hi - 1) / child_bs;
    for (uint64_t c = c0; c <= c1; ++c)
        walk_block(ch, level - 1, c, vfirst, vlast, cur, out, blocks_scanned);
}

void LogicStore::walk(int ch, uint64_t first, uint64_t last, double spp,
                      uint8_t &initial, std::vector<Edge> &out,
                      uint64_t *blocks_scanned) const {
    std::shared_lock<std::shared_mutex> lk(reclaim_mtx_);
    out.clear();
    if (blocks_scanned) *blocks_scanned = 0;
    uint64_t lo = std::max(first, first_live_.load(std::memory_order_acquire));
    uint64_t hi = std::min(last, count_.load(std::memory_order_acquire));
    if (hi <= lo) { initial = 0; return; }
    initial = bit(ch, lo);
    uint8_t cur = initial;
    int top = pyr_top_level(spp);
    uint64_t tbs = pyr_block_size(top);
    uint64_t b0 = lo / tbs;
    uint64_t b1 = (hi - 1) / tbs;
    for (uint64_t b = b0; b <= b1; ++b)
        walk_block(ch, top, b, lo, hi, cur, out, blocks_scanned);
}

// ======================= AnalogStore =======================
AnalogStore::AnalogStore() {
    for (size_t i = 0; i < SLOTS; ++i) chunks_[i].store(nullptr, std::memory_order_relaxed);
}
AnalogStore::~AnalogStore() {
    for (size_t i = 0; i < SLOTS; ++i) delete[] chunks_[i].load(std::memory_order_relaxed);
}
void AnalogStore::reset() {
    for (size_t i = 0; i < SLOTS; ++i)
        delete[] chunks_[i].exchange(nullptr, std::memory_order_relaxed);
    written_end_ = 0;
    count_.store(0, std::memory_order_release);
}
float *AnalogStore::ensure(uint64_t s) {
    size_t slot = (size_t)(s / SPC);
    if (slot >= SLOTS) return nullptr;  // capacity limit: never write out of bounds
    float *c = chunks_[slot].load(std::memory_order_relaxed);
    if (!c) {
        c = new float[(size_t)SPC]();
        chunks_[slot].store(c, std::memory_order_release);
    }
    return c + (size_t)(s % SPC);
}
const float *AnalogStore::read(uint64_t s) const {
    size_t slot = (size_t)(s / SPC);
    if (slot >= SLOTS) return nullptr;
    float *c = chunks_[slot].load(std::memory_order_acquire);
    if (!c) return nullptr;
    return c + (size_t)(s % SPC);
}
void AnalogStore::append(uint64_t start, uint32_t count, const float *data) {
    if (count == 0) return;
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        float *p = ensure(start + i);
        if (!p) break;  // capacity limit reached: drop the rest, stay safe
        *p = data[i];
        ++written;
    }
    if ((count = written) == 0) return;
    if (start + count > written_end_) written_end_ = start + count;

    const float INF = std::numeric_limits<float>::infinity();
    uint64_t b0 = start / 16, b1 = (start + count - 1) / 16;
    for (uint64_t b = b0; b <= b1; ++b) {
        uint64_t s0 = b * 16, s1 = std::min(s0 + 16, written_end_);
        float mn = INF, mx = -INF;
        for (uint64_t s = s0; s < s1; ++s) {
            float v = value(s);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (mn <= mx) env_.set_cell(0, b, mn, mx);
    }
    uint64_t lo = b0, hi = b1;
    for (int level = 1; level < PYR_LEVELS; ++level) {
        lo >>= 4; hi >>= 4;
        for (uint64_t pb = lo; pb <= hi; ++pb) {
            float mn = INF, mx = -INF; bool any = false;
            for (int k = 0; k < 16; ++k) {
                float cmn, cmx;
                if (env_.get_cell(level - 1, pb * 16 + k, cmn, cmx)) {
                    any = true; mn = std::min(mn, cmn); mx = std::max(mx, cmx);
                }
            }
            if (any) env_.set_cell(level, pb, mn, mx);
        }
    }
    count_.store(written_end_, std::memory_order_release);
}
bool AnalogStore::envelope(uint64_t s, double spp, float &mn, float &mx) const {
    if (spp < 16.0) return false;  // caller draws a true polyline
    int level = pyr_top_level(spp);
    return env_.get_cell(level, s / pyr_block_size(level), mn, mx);
}

// ======================= AnnotationStore =======================
void AnnotationStore::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    rows_.clear();
    total_ = 0;
}
void AnnotationStore::add(uint32_t stack_id, uint16_t row_id, const Annotation &a) {
    std::lock_guard<std::mutex> lk(mtx_);
    Key k{stack_id, row_id};
    std::vector<Annotation> *v = nullptr;
    for (auto &r : rows_) {
        if (!(r.first < k) && !(k < r.first)) { v = &r.second; break; }
    }
    if (!v) {
        rows_.push_back({k, {}});
        v = &rows_.back().second;
    }
    // Keep each row sorted by start so visit() can binary-search. Decoders emit
    // in time order, so out-of-order inserts are rare.
    if (!v->empty() && a.start < v->back().start) {
        auto it = std::lower_bound(v->begin(), v->end(), a.start,
                                   [](const Annotation &x, uint64_t s) { return x.start < s; });
        v->insert(it, a);
    } else {
        v->push_back(a);
    }
    ++total_;
}
const std::vector<Annotation> *AnnotationStore::row_for(uint32_t stack, uint16_t row) const {
    Key k{stack, row};
    for (auto &r : rows_)
        if (!(r.first < k) && !(k < r.first)) return &r.second;
    return nullptr;
}
void AnnotationStore::trim_before(uint64_t sample) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &r : rows_) {
        auto &v = r.second;
        size_t dead = 0;
        while (dead < v.size() && v[dead].end < sample) ++dead;
        if (dead >= 1024 || dead == v.size()) {  // amortise the erase
            v.erase(v.begin(), v.begin() + dead);
            total_ -= dead;
        }
    }
}
size_t AnnotationStore::total() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return total_;
}

}  // namespace son
