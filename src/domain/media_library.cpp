#include "media_library.h"

#include "util/logger.h"

#include <sys/stat.h>

#include <cstdio>

namespace vedit {

namespace {

std::string basename(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

double fps_from_rational(int num, int den) {
    return (den != 0) ? static_cast<double>(num) / den : 0.0;
}

} // namespace

MediaLibrary::MediaLibrary(Mlt::Profile& profile) : profile_(profile) {}

MediaLibrary::~MediaLibrary() = default;

int MediaLibrary::import(const std::string& path) {
    auto found = by_path_.find(path);
    if (found != by_path_.end()) return found->second;

    auto producer = std::make_unique<Mlt::Producer>(profile_, path.c_str());
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Error, "MediaLibrary: failed to open");
        return -1;
    }

    MediaItem item;
    item.path = path;
    item.name = basename(path);
    item.frame_count = producer->get_length();
    item.has_video = producer->get_int("video_index") >= 0;
    item.has_audio = producer->get_int("audio_index") >= 0;

    // Probe stream metadata via the underlying properties.
    Mlt::Properties props(producer->get_properties());
    if (item.has_video) {
        // Source dimensions live on a decoded frame's meta.media.* keys.
        // Producer-level meta.media.* may be 0 before any frame is pulled,
        // and get_image() returns profile-scaled dimensions, not source.
        Mlt::Frame* frame = producer->get_frame();
        if (frame && frame->is_valid()) {
            int w = frame->get_int("meta.media.width");
            int h = frame->get_int("meta.media.height");
            if (w <= 0) w = producer->get_int("meta.media.width");
            if (h <= 0) h = producer->get_int("meta.media.height");
            item.width = w;
            item.height = h;
            delete frame;
        }
        int fps_num = producer->get_int("meta.media.frame_rate_num");
        int fps_den = producer->get_int("meta.media.frame_rate_den");
        item.fps = fps_from_rational(fps_num, fps_den);
        if (item.fps <= 0 && item.frame_count > 0) {
            item.fps = static_cast<double>(producer->get_double("fps"));
        }
    }
    if (item.fps > 0 && item.frame_count > 0) {
        item.duration_sec = item.frame_count / item.fps;
    }

    int index = static_cast<int>(items_.size());
    items_.push_back(std::move(item));
    producers_.push_back(std::move(producer));
    by_path_[path] = index;
    return index;
}

const MediaItem* MediaLibrary::item(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return nullptr;
    return &items_[index];
}

Mlt::Producer* MediaLibrary::producer(int index) {
    if (index < 0 || index >= static_cast<int>(producers_.size())) return nullptr;
    return producers_[index].get();
}

} // namespace vedit
