// Headless self-test of the sample store + transition pyramid + decimated walk.
// No SDL/GL. Run with: sonview --unittest
#include <cstdint>
#include <cstdio>
#include <vector>

#include "stores.h"

namespace son {

static int g_fail = 0;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL: " __VA_ARGS__);                  \
            std::printf("   (%s:%d)\n", __FILE__, __LINE__);    \
            ++g_fail;                                           \
        }                                                       \
    } while (0)

int run_unittest() {
    std::printf("== sonview --unittest ==\n");

    const uint64_t TOTAL = 1000000;
    const uint64_t P0 = 1000;   // ch0 toggles every 1000 samples
    const uint64_t P1 = 8000;   // ch1 toggles every 8000 samples
    const uint64_t GLITCH = 250123;  // 1-sample glitch on ch0 (stable region)

    auto v0 = [&](uint64_t s) -> int {
        int b = (int)((s / P0) & 1);
        if (s == GLITCH) b ^= 1;  // single-sample glitch
        return b;
    };
    auto v1 = [&](uint64_t s) -> int { return (int)((s / P1) & 1); };

    // Build bit-packed samples (bit0 = ch0, bit1 = ch1), unitsize = 1.
    std::vector<uint8_t> data(TOTAL);
    for (uint64_t s = 0; s < TOTAL; ++s)
        data[s] = (uint8_t)(v0(s) | (v1(s) << 1));

    // Ground-truth edge lists (samples where value differs from previous).
    std::vector<uint64_t> gt0, gt1;
    for (uint64_t s = 1; s < TOTAL; ++s) {
        if (v0(s) != v0(s - 1)) gt0.push_back(s);
        if (v1(s) != v1(s - 1)) gt1.push_back(s);
    }
    std::printf("ground truth: ch0=%zu transitions, ch1=%zu transitions\n",
                gt0.size(), gt1.size());
    // Glitch adds exactly two transitions to ch0.
    CHECK(gt0.size() == (TOTAL - 1) / P0 + 2,
          "ch0 transition count %zu unexpected\n", gt0.size());

    // Feed through the store in several chunks (exercises cross-chunk transitions
    // and the chunked, non-reallocating storage).
    LogicStore store;
    store.reset(1);
    store.set_tracked_bits({0, 1});
    const uint64_t STEP = 137000;  // deliberately not a chunk multiple
    for (uint64_t off = 0; off < TOTAL; off += STEP) {
        uint32_t n = (uint32_t)std::min<uint64_t>(STEP, TOTAL - off);
        store.append(off, n, data.data() + off, /*discontinuity=*/false);
    }
    CHECK(store.count() == TOTAL, "store.count()=%llu\n",
          (unsigned long long)store.count());

    // --- Pyramid reports the correct transition count, and DECIMATES ---------
    {
        std::vector<Edge> e0, e1;
        uint8_t init0 = 9, init1 = 9;
        uint64_t scanned0 = 0, scanned1 = 0;
        // Large spp => zoomed all the way out; must still find every edge.
        store.walk(0, 0, TOTAL, 1e6, init0, e0, &scanned0);
        store.walk(1, 0, TOTAL, 1e6, init1, e1, &scanned1);
        CHECK(init0 == (uint8_t)v0(0), "ch0 initial value wrong\n");
        CHECK(init1 == (uint8_t)v1(0), "ch1 initial value wrong\n");
        CHECK(e0.size() == gt0.size(),
              "ch0 pyramid edge count %zu != %zu (zoomed out)\n", e0.size(), gt0.size());
        CHECK(e1.size() == gt1.size(),
              "ch1 pyramid edge count %zu != %zu (zoomed out)\n", e1.size(), gt1.size());

        uint64_t total_blocks = (TOTAL + 15) / 16;
        // Decimation: transitioning level-0 blocks scanned must be << all blocks,
        // and cannot exceed the number of edges found.
        std::printf("decimation @spp=1e6: ch0 scanned %llu/%llu blocks, "
                    "ch1 scanned %llu/%llu blocks\n",
                    (unsigned long long)scanned0, (unsigned long long)total_blocks,
                    (unsigned long long)scanned1, (unsigned long long)total_blocks);
        CHECK(scanned0 <= e0.size() && scanned0 >= 1, "ch0 decimation bound\n");
        CHECK(scanned1 <= e1.size() && scanned1 >= 1, "ch1 decimation bound\n");
        CHECK(scanned0 * 10 < total_blocks, "ch0 did not decimate quiet blocks\n");
        CHECK(scanned1 * 10 < total_blocks, "ch1 did not decimate quiet blocks\n");
    }

    // --- Edge count invariant across many zoom levels (glitch survives) -------
    const double spps[] = {1, 4, 16, 100, 1000, 10000, 100000, 1e6, 1e7};
    for (double spp : spps) {
        std::vector<Edge> e0, e1;
        uint8_t i0, i1;
        store.walk(0, 0, TOTAL, spp, i0, e0, nullptr);
        store.walk(1, 0, TOTAL, spp, i1, e1, nullptr);
        CHECK(e0.size() == gt0.size(),
              "ch0 edges=%zu != %zu at spp=%g\n", e0.size(), gt0.size(), spp);
        CHECK(e1.size() == gt1.size(),
              "ch1 edges=%zu != %zu at spp=%g\n", e1.size(), gt1.size(), spp);
        // Verify exact edge samples at the coarsest zoom (glitch must be there).
        if (spp >= 1e6) {
            bool ok = (e0.size() == gt0.size());
            for (size_t i = 0; ok && i < e0.size(); ++i)
                ok = (e0[i].sample == gt0[i]);
            CHECK(ok, "ch0 exact edge samples mismatch at spp=%g\n", spp);
            bool found_glitch = false;
            for (auto &e : e0)
                if (e.sample == GLITCH || e.sample == GLITCH + 1) found_glitch = true;
            CHECK(found_glitch, "1-sample glitch lost at spp=%g\n", spp);
        }
    }

    // --- Windowed walk around the glitch -------------------------------------
    {
        std::vector<Edge> e;
        uint8_t init;
        store.walk(0, GLITCH - 5, GLITCH + 5, 1.0, init, e, nullptr);
        CHECK(e.size() == 2, "windowed glitch walk got %zu edges (want 2)\n", e.size());
        if (e.size() == 2) {
            CHECK(e[0].sample == GLITCH && e[0].value == (uint8_t)(v0(GLITCH)),
                  "glitch rising edge wrong\n");
            CHECK(e[1].sample == GLITCH + 1, "glitch falling edge wrong\n");
        }
    }

    // --- Exact edge samples at 1 sample/pixel over the full range -------------
    {
        std::vector<Edge> e;
        uint8_t init;
        store.walk(0, 0, TOTAL, 1.0, init, e, nullptr);
        bool ok = (e.size() == gt0.size());
        for (size_t i = 0; ok && i < e.size(); ++i) ok = (e[i].sample == gt0[i]);
        CHECK(ok, "ch0 exact edges at spp=1 mismatch\n");
    }

    // --- Analog store + envelope sanity --------------------------------------
    {
        AnalogStore a;
        a.reset();
        const uint64_t AN = 100000;
        std::vector<float> af(AN);
        for (uint64_t s = 0; s < AN; ++s)
            af[s] = (s == 54321) ? 99.0f : (float)((s / 200) % 2);  // 0/1 square + spike
        a.append(0, (uint32_t)AN, af.data());
        CHECK(a.count() == AN, "analog count wrong\n");
        float mn, mx;
        // Zoomed out: the block covering the spike must report max ~99 (survives).
        bool got = a.envelope(54321, 4096.0, mn, mx);
        CHECK(got && mx >= 99.0f, "analog spike lost in envelope (mx=%g)\n",
              got ? mx : -1.0);
    }

    // --- 2^32-sample capacity: writes at/near the limit must be safe -----------
    {
        LogicStore ls;
        ls.reset(1);
        uint8_t blk[256];
        for (int i = 0; i < 256; ++i) blk[i] = (uint8_t)(i & 1);
        const uint64_t CAPS = LogicStore::capacity();
        ls.append(CAPS - 128, 256, blk, true);  // straddles the capacity boundary
        CHECK(ls.full(), "store should report full at capacity\n");
        CHECK(ls.count() <= CAPS, "count exceeded capacity (%llu)\n",
              (unsigned long long)ls.count());
        ls.append(CAPS + 1000, 64, blk, true);  // fully beyond: must not crash
        CHECK(ls.count() <= CAPS, "post-capacity append advanced count\n");
    }

    // --- AnnotationStore: sorted visit + trim ----------------------------------
    {
        AnnotationStore as;
        as.reset();
        for (uint64_t i = 0; i < 5000; ++i) {
            Annotation a;
            a.start = i * 100;
            a.end = i * 100 + 150;  // overlaps the next one (exercises back-scan)
            a.ann_class = (uint16_t)(i % 4);
            a.texts = {"t" + std::to_string(i)};
            as.add(0, 0, a);
        }
        size_t seen = 0;
        uint64_t first = 0, last = 0;
        as.visit(0, 0, 10000, 20000, [&](const Annotation &a) {
            if (!seen) first = a.start;
            last = a.start;
            ++seen;
        });
        // starts in [10000, 20000] (101) + the i=99 one overlapping in from 9900
        CHECK(seen == 102, "visit count %zu (want 102)\n", seen);
        CHECK(first == 9900 && last == 20000, "visit range [%llu,%llu]\n",
              (unsigned long long)first, (unsigned long long)last);
        as.trim_before(250000);  // drop annotations ending (<i*100+150) before it
        size_t left = as.total();
        CHECK(left == 2501, "trim left %zu (want 2501)\n", left);
    }

    if (g_fail == 0)
        std::printf("\nALL TESTS PASSED\n");
    else
        std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}

}  // namespace son
