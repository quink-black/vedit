#include "project_model.h"

#include <numeric>  // std::gcd

#include "util/logger.h"

namespace vedit {

ProjectModel::ProjectModel()
    : library_(std::make_unique<MediaLibrary>(profile_)),
      timeline_(std::make_unique<TimelineModel>(profile_, *library_)) {
    // MLT's default profile (DV/DVD PAL) is interlaced (progressive=0),
    // which makes consumer_avformat / libx264 refuse to encode with
    // "interlace + weightp is not implemented" and produces an empty
    // file. Force progressive scan — vedit is a progressive editor.
    profile_.set_progressive(1);
}

ProjectModel::~ProjectModel() = default;

void ProjectModel::set_profile(int width, int height, double fps) {
    if (width > 0) profile_.set_width(width);
    if (height > 0) profile_.set_height(height);
    profile_.set_progressive(1);
    // MLT's default profile (DV PAL) has non-square pixels
    // (SAR 16:15). When the user sets a custom canvas resolution,
    // square pixels are the expected default. Without this, the
    // rescale pipeline computes the wrong output dimensions (e.g.,
    // a 1280x480 canvas with SAR 16:15 produces a 640x480 export).
    profile_.set_sample_aspect(1, 1);
    if (width > 0 && height > 0) {
        int gcd = std::gcd(width, height);
        profile_.set_display_aspect(width / gcd, height / gcd);
    }
    if (fps > 0) {
        // Convert fps to a rational (frame_rate_num / frame_rate_den).
        // MLT stores fps as numerator/denominator; approximate the
        // double as a rational with den=1001 for common NTSC rates or
        // den=1 for integer rates.
        int den = 1;
        double frac = fps - static_cast<int>(fps);
        if (frac > 0.029 && frac < 0.031) {
            // 29.97 / 59.94 / 23.976 etc. — use 1001 denominator
            den = 1001;
        }
        profile_.set_frame_rate(
            static_cast<int>(fps * den + 0.5), den);
    }
    profile_user_set_ = true;
}

} // namespace vedit
