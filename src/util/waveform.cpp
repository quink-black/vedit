#include "waveform.h"

#include <algorithm>
#include <cmath>

namespace vedit {

std::vector<float> compute_peaks(const int16_t* samples, size_t sample_count,
                                 int channels, int bins) {
    std::vector<float> peaks;
    if (bins <= 0 || channels <= 0 || samples == nullptr || sample_count == 0)
        return peaks;

    peaks.assign(static_cast<size_t>(bins), 0.0f);

    // Frames are interleaved; one frame is `channels` samples. Mixing the
    // channels by taking the loudest of them keeps a stereo peak from
    // reading as half height just because the two ears differ.
    const size_t frames = sample_count / static_cast<size_t>(channels);
    if (frames == 0) return peaks;

    const float frames_per_bin = static_cast<float>(frames) / bins;
    constexpr float kFullScale = 32768.0f;

    for (size_t frame = 0; frame < frames; ++frame) {
        int bin = static_cast<int>(static_cast<float>(frame) / frames_per_bin);
        if (bin >= bins) bin = bins - 1;
        const int16_t* src = samples + frame * static_cast<size_t>(channels);
        int16_t loudest = 0;
        for (int ch = 0; ch < channels; ++ch) {
            int16_t v = static_cast<int16_t>(src[ch] < 0 ? -src[ch] : src[ch]);
            if (v > loudest) loudest = v;
        }
        float normalised = static_cast<float>(loudest) / kFullScale;
        if (normalised > peaks[static_cast<size_t>(bin)])
            peaks[static_cast<size_t>(bin)] = normalised;
    }
    return peaks;
}

} // namespace vedit
