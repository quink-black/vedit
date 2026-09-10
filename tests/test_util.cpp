#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "util/waveform.h"

using vedit::compute_peaks;

TEST_CASE("compute_peaks folds samples into bins and normalises", "[util]") {
    // One second of stereo silence at 25 frames: peaks must be 0.
    std::vector<int16_t> silence(500 * 2, 0);
    auto p = compute_peaks(silence.data(), silence.size(), 2, 10);
    REQUIRE(p.size() == 10);
    for (float v : p) REQUIRE(v == 0.0f);

    // A full-scale sine: every bin that samples land in must reach 1.0.
    std::vector<int16_t> loud(500 * 2);
    for (size_t i = 0; i < loud.size(); ++i)
        loud[i] = static_cast<int16_t>(32000 * std::sin(i * 0.1));
    p = compute_peaks(loud.data(), loud.size(), 2, 10);
    for (float v : p) REQUIRE(v > 0.9f);

    // A single full-scale sample lands in exactly one bin, which reads
    // full height while the rest stay silent. This is the property that
    // makes the drawing correspond to the samples rather than to the
    // bin count.
    std::vector<int16_t> sparse(1000, 0);
    sparse[500] = 32767;
    p = compute_peaks(sparse.data(), sparse.size(), 1, 100);
    REQUIRE(p.size() == 100);
    int hot = 0;
    for (float v : p) {
        if (v > 0.99f) ++hot;
        else REQUIRE(v == 0.0f);
    }
    REQUIRE(hot == 1);
}

TEST_CASE("compute_peaks rejects degenerate input", "[util]") {
    std::vector<int16_t> samples(100, 100);
    REQUIRE(compute_peaks(nullptr, 100, 1, 10).empty());
    REQUIRE(compute_peaks(samples.data(), 0, 1, 10).empty());
    REQUIRE(compute_peaks(samples.data(), 100, 0, 10).empty());
    REQUIRE(compute_peaks(samples.data(), 100, 1, 0).empty());
}
