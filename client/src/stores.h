// Sample + annotation stores. Single writer (RX thread) appends; the render
// thread reads lock-free up to the published atomic count. Chunks never move.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "pyramid.h"

namespace son {

// ---- logic --------------------------------------------------------------
struct Edge {
    uint64_t sample;  // sample index at which the level changes
    uint8_t value;    // new level (0/1) at/after this sample
};

class LogicStore {
public:
    static constexpr uint64_t SPC = 1u << 16;      // samples per chunk
    static constexpr size_t SLOTS = 1u << 16;      // -> up to 2^32 samples

    LogicStore();
    ~LogicStore();
    LogicStore(const LogicStore &) = delete;
    LogicStore &operator=(const LogicStore &) = delete;

    void reset(uint8_t unitsize);
    void set_tracked_bits(const std::vector<int> &bits);  // bits to build pyramids for

    // Append a contiguous run of bit-packed sample words. `discontinuity` marks
    // a gap before this chunk (no transition computed across it).
    void append(uint64_t start_sample, uint32_t count, const uint8_t *data,
                bool discontinuity);

    uint8_t unitsize() const { return unitsize_; }
    uint64_t count() const { return count_.load(std::memory_order_acquire); }
    uint64_t first_live() const { return first_live_.load(std::memory_order_acquire); }
    // True once appends were dropped at the 2^32-sample capacity limit.
    bool full() const { return full_.load(std::memory_order_relaxed); }
    static constexpr uint64_t capacity() { return SPC * (uint64_t)SLOTS; }

    // Bounded rolling window for continuous mode (0 = unbounded / keep all).
    void set_max_samples(uint64_t m) { max_samples_ = m; }

    // Copy raw bit-packed sample words [start, start+count) into out (holes
    // read as zero). For exports and re-decoding.
    void copy_raw(uint64_t start, uint32_t count, uint8_t *out) const {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *p = word_read(start + i);
            if (p)
                for (int k = 0; k < unitsize_; ++k) out[(size_t)i * unitsize_ + k] = p[k];
            else
                for (int k = 0; k < unitsize_; ++k) out[(size_t)i * unitsize_ + k] = 0;
        }
    }

    // Value of `bit` at absolute sample s (0 if the sample is in a hole).
    uint8_t bit(int b, uint64_t s) const {
        const uint8_t *p = word_read(s);
        if (!p) return 0;
        uint64_t w = 0;
        for (int i = 0; i < unitsize_; ++i) w |= (uint64_t)p[i] << (8 * i);
        return (uint8_t)((w >> b) & 1u);
    }

    const BitPyramid *pyramid_for(int bit) const;

    // Decimated transition walk over [first,last) for `bit`, choosing a pyramid
    // level from spp. Emits every real edge (glitches survive). O(pixels) over
    // quiet regions. `initial` = level at `first`. Takes a shared lock so the
    // range cannot be reclaimed underneath us.
    void walk(int bit, uint64_t first, uint64_t last, double spp,
              uint8_t &initial, std::vector<Edge> &out,
              uint64_t *blocks_scanned = nullptr) const;

    // Copy of discontinuity boundaries (absolute sample of each gap start).
    std::vector<uint64_t> gaps() const;

private:
    uint8_t *ensure_word(uint64_t s);
    const uint8_t *word_read(uint64_t s) const;
    void trim();

    uint8_t unitsize_ = 0;
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> first_live_{0};
    std::atomic<bool> full_{false};
    uint64_t written_end_ = 0;
    uint64_t max_samples_ = 0;
    std::atomic<uint8_t *> chunks_[SLOTS];

    std::vector<int> tracked_;
    BitPyramid *pyr_[64] = {nullptr};  // indexed by bit position

    mutable std::shared_mutex reclaim_mtx_;  // held unique while freeing chunks
    mutable std::mutex gaps_mtx_;
    std::vector<uint64_t> gaps_;

    // recursive walk helper
    void walk_block(int bit, int level, uint64_t block, uint64_t first,
                    uint64_t last, uint8_t &cur, std::vector<Edge> &out,
                    uint64_t *blocks_scanned) const;
};

// ---- analog -------------------------------------------------------------
class AnalogStore {
public:
    static constexpr uint64_t SPC = 1u << 16;
    static constexpr size_t SLOTS = 1u << 16;

    AnalogStore();
    ~AnalogStore();
    AnalogStore(const AnalogStore &) = delete;
    AnalogStore &operator=(const AnalogStore &) = delete;

    void reset();
    void append(uint64_t start_sample, uint32_t count, const float *data);

    uint64_t count() const { return count_.load(std::memory_order_acquire); }
    float value(uint64_t s) const {
        const float *p = read(s);
        return p ? *p : 0.0f;
    }
    // Envelope (min/max) for the block covering sample s at the level whose
    // block-size best matches spp. Returns false if unavailable.
    bool envelope(uint64_t s, double spp, float &mn, float &mx) const;

private:
    float *ensure(uint64_t s);
    const float *read(uint64_t s) const;

    std::atomic<uint64_t> count_{0};
    uint64_t written_end_ = 0;
    std::atomic<float *> chunks_[SLOTS];
    EnvPyramid env_;
};

// ---- annotations --------------------------------------------------------
struct Annotation {
    uint64_t start, end;
    uint16_t ann_class;
    std::vector<std::string> texts;  // longest first
};

class AnnotationStore {
public:
    void reset();
    void add(uint32_t stack_id, uint16_t row_id, const Annotation &a);
    size_t total() const;

    // Visit annotations of one row overlapping [lo, hi] in start order, without
    // copying. Rows are kept sorted by start, so this is O(log N + visible).
    // The callback runs under the store mutex — keep it lightweight.
    template <class F>
    void visit(uint32_t stack_id, uint16_t row_id, uint64_t lo, uint64_t hi,
               F &&fn) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::vector<Annotation> *v = row_for(stack_id, row_id);
        if (!v || v->empty()) return;
        auto it = std::lower_bound(v->begin(), v->end(), lo,
                                   [](const Annotation &a, uint64_t s) { return a.start < s; });
        // Back up over annotations that start before lo but overlap into it.
        while (it != v->begin() && (it - 1)->end >= lo) --it;
        for (; it != v->end() && it->start <= hi; ++it) fn(*it);
    }

    // Visit every annotation of every row (for tables / CSV export).
    template <class F>
    void for_each(F &&fn) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto &r : rows_)
            for (const Annotation &a : r.second) fn(r.first.stack, r.first.row, a);
    }

    // Rolling window: drop annotations that end before `sample`.
    void trim_before(uint64_t sample);

private:
    struct Key {
        uint32_t stack;
        uint16_t row;
        bool operator<(const Key &o) const {
            return stack != o.stack ? stack < o.stack : row < o.row;
        }
    };
    const std::vector<Annotation> *row_for(uint32_t stack, uint16_t row) const;
    mutable std::mutex mtx_;
    std::vector<std::pair<Key, std::vector<Annotation>>> rows_;
    size_t total_ = 0;
};

}  // namespace son
