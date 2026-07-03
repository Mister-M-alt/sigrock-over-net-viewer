// Lock-free, non-reallocating chunked array.
//
// A fixed-size table of atomic chunk pointers. The single writer allocates a
// chunk lazily and publishes the pointer with release semantics; readers load
// the pointer with acquire semantics. Because the pointer table itself is sized
// once at construction (never resized) and the chunks it points at never move,
// a reader can safely access any element whose index is < a separately-published
// (release/acquire) element count, with no locking.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace son {

// T must be trivially usable (we do not run element ctors across chunk memory
// beyond value-init). CHUNK = elements per chunk, SLOTS = max chunks.
template <class T, size_t CHUNK, size_t SLOTS>
class Chunked {
public:
    Chunked() {
        for (size_t i = 0; i < SLOTS; ++i)
            slots_[i].store(nullptr, std::memory_order_relaxed);
    }
    ~Chunked() {
        for (size_t i = 0; i < SLOTS; ++i) {
            T *p = slots_[i].load(std::memory_order_relaxed);
            delete[] p;
        }
    }
    Chunked(const Chunked &) = delete;
    Chunked &operator=(const Chunked &) = delete;

    static constexpr size_t chunk_elems() { return CHUNK; }
    static constexpr size_t max_elems() { return CHUNK * SLOTS; }

    // Writer: make sure the chunk holding `index` exists. Returns pointer to the
    // element, or nullptr when `index` is beyond capacity (caller must handle —
    // silently indexing past SLOTS would corrupt memory).
    // Not thread-safe against other writers (single writer assumed).
    T *ensure(uint64_t index) {
        size_t s = (size_t)(index / CHUNK);
        if (s >= SLOTS) return nullptr;
        T *c = slots_[s].load(std::memory_order_relaxed);
        if (!c) {
            c = new T[CHUNK]();  // value-initialised (zeroed for scalars)
            slots_[s].store(c, std::memory_order_release);
        }
        return &c[index % CHUNK];
    }

    // Writer: release every chunk that lies entirely below `index` (rolling
    // window). Caller must exclude readers (e.g. via its reclaim lock).
    void free_below(uint64_t index) {
        size_t upto = (size_t)(index / CHUNK);
        if (upto > SLOTS) upto = SLOTS;
        for (size_t s = freed_upto_; s < upto; ++s)
            delete[] slots_[s].exchange(nullptr, std::memory_order_relaxed);
        if (upto > freed_upto_) freed_upto_ = upto;
    }

    // Reader: element must belong to an already-allocated chunk (index < count
    // that was published after the writer touched it). Returns nullptr if the
    // chunk is absent (e.g. a gap/hole left by a discontinuity).
    T *get(uint64_t index) const {
        size_t s = (size_t)(index / CHUNK);
        if (s >= SLOTS) return nullptr;
        T *c = slots_[s].load(std::memory_order_acquire);
        if (!c) return nullptr;
        return &c[index % CHUNK];
    }

    bool has_chunk_for(uint64_t index) const {
        size_t s = (size_t)(index / CHUNK);
        return s < SLOTS && slots_[s].load(std::memory_order_acquire) != nullptr;
    }

private:
    std::atomic<T *> slots_[SLOTS];
    size_t freed_upto_ = 0;  // writer-only: low-water mark for free_below
};

}  // namespace son
