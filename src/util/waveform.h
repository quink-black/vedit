#ifndef VEDIT_APP_WAVEFORM_H
#define VEDIT_APP_WAVEFORM_H

#include <cstddef>
#include <vector>

namespace vedit {

// Pure sample math, so the drawing layer can be tested without MLT.

// Peak envelope for drawing a waveform.
//
// The timeline needs a fixed number of columns per clip regardless of how
// long the clip is, so the source samples are folded into `bins` columns.
// Each column holds the largest absolute sample it contains, normalised
// to 0..1 against 16-bit full scale, which is what makes the drawing
// correspond to what the ear hears rather than to the sample count.
std::vector<float> compute_peaks(const int16_t* samples, size_t sample_count,
                                 int channels, int bins);

} // namespace vedit

#endif
