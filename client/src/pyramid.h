// Transition-OR mip pyramid (logic) and min/max envelope pyramid (analog).
//
// Factor R = 16, up to LEVELS levels. blockSize(L) = 16^(L+1) samples.
//   Logic  : level-0 flag[b] = "channel changed within samples [b*16, b*16+16)"
//            (OR of xor-with-previous). Higher levels OR their children.
//   Analog : level-0 cell[b] = min/max over samples [b*16, b*16+16). Higher
//            levels combine children.
// Both are grown by a single writer and read lock-free (atomic cells; the caller
// publishes a sample count with release after touching the pyramid).
#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

#include "chunked.h"

namespace son {

static constexpr int PYR_LEVELS = 8;               // covers 16^8 = 2^32 samples
static constexpr size_t PYR_CHUNK = 1u << 16;      // flags/cells per chunk
static constexpr size_t PYR_SLOTS = 1u << 13;      // -> up to 2^29 entries/level

// blockSize(L) = 16^(L+1) = 2^(4*(L+1)).
static inline uint64_t pyr_block_size(int level) {
    return (uint64_t)1 << (4 * (level + 1));
}

// Largest level whose blocks are still <= spp (so each block maps to <=1 pixel).
static inline int pyr_top_level(double spp) {
    int L = 0;
    while (L + 1 < PYR_LEVELS && (double)pyr_block_size(L + 1) <= spp) ++L;
    return L;
}

// ---- per-channel transition pyramid -------------------------------------
class BitPyramid {
public:
    // Record that a transition occurred *at* `sample` (sample != sample-1).
    // Caller guarantees sample >= 1.
    void set_transition(uint64_t sample) {
        uint64_t idx = sample >> 4;  // / 16
        for (int L = 0; L < PYR_LEVELS; ++L) {
            std::atomic<uint8_t> *cell = lvl_[L].ensure(idx);
            if (!cell) return;  // beyond capacity: drop (matches store behaviour)
            if (cell->load(std::memory_order_relaxed)) break;  // ancestors set too
            cell->store(1, std::memory_order_relaxed);
            idx >>= 4;
        }
    }
    uint8_t flag(int level, uint64_t block_index) const {
        std::atomic<uint8_t> *c = lvl_[level].get(block_index);
        return c ? c->load(std::memory_order_relaxed) : 0;
    }
    // Rolling window: release flag chunks entirely below `first_live_sample`.
    // Caller must exclude readers (LogicStore holds its reclaim lock).
    void trim(uint64_t first_live_sample) {
        uint64_t idx = first_live_sample >> 4;
        for (int L = 0; L < PYR_LEVELS; ++L) {
            lvl_[L].free_below(idx);
            idx >>= 4;
        }
    }

private:
    Chunked<std::atomic<uint8_t>, PYR_CHUNK, PYR_SLOTS> lvl_[PYR_LEVELS];
};

// ---- per-channel analog envelope pyramid --------------------------------
class EnvPyramid {
public:
    void set_cell(int level, uint64_t idx, float mn, float mx) {
        std::atomic<float> *c = mn_[level].ensure(idx);
        c->store(mn, std::memory_order_relaxed);
        mx_[level].ensure(idx)->store(mx, std::memory_order_relaxed);
    }
    bool get_cell(int level, uint64_t idx, float &mn, float &mx) const {
        std::atomic<float> *cn = mn_[level].get(idx);
        std::atomic<float> *cx = mx_[level].get(idx);
        if (!cn || !cx) return false;
        mn = cn->load(std::memory_order_relaxed);
        mx = cx->load(std::memory_order_relaxed);
        return true;
    }

private:
    Chunked<std::atomic<float>, PYR_CHUNK, PYR_SLOTS> mn_[PYR_LEVELS];
    Chunked<std::atomic<float>, PYR_CHUNK, PYR_SLOTS> mx_[PYR_LEVELS];
};

}  // namespace son
