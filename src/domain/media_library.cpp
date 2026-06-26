#include "media_library.h"

#include "util/logger.h"

#include <cstdio>
#include <cstring>

namespace vedit {

namespace {

std::string basename(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

double fps_from_rational(int num, int den) {
    return (den != 0) ? static_cast<double>(num) / den : 0.0;
}

// Apply the caller's stream selection to a freshly-opened producer.
// `audio_index == -2` is our sentinel for "all audio streams" — MLT
// accepts the string "all" for the audio_index property (producer_avformat.c
// in mlt/src/modules/avformat). All other values pass through as ints.
void apply_stream_selection(Mlt::Producer& producer, int video_index, int audio_index) {
    if (video_index >= 0) {
        producer.set("video_index", video_index);
    }
    if (audio_index == -2) {
        producer.set("audio_index", "all");
    } else if (audio_index >= 0) {
        producer.set("audio_index", audio_index);
    }
}

// Detect subtitle streams when MLT did not write stream.type for them.
// producer_avformat's codec-type switch has an empty `default:` case,
// so subtitle / data streams have no `meta.media.<i>.stream.type`.
// Fall back to scanning codec.long_name for the word "subtitle".
bool looks_like_subtitle(const std::string& long_name) {
    if (long_name.empty()) return false;
    // Case-insensitive substring search; "subtitle" appears in e.g.
    // "Timed text subtitle" / "ASS (Advanced SubStation Alpha) subtitle".
    auto pos = long_name.find("subtitle");
    if (pos == std::string::npos) pos = long_name.find("Subtitle");
    return pos != std::string::npos;
}

} // namespace

MediaLibrary::MediaLibrary(Mlt::Profile& profile) : profile_(profile) {}

MediaLibrary::~MediaLibrary() = default;

void MediaLibrary::probe_streams(MediaItem& item, Mlt::Producer& producer) {
    Mlt::Properties props(producer.get_properties());
    int nb = producer.get_int("meta.media.nb_streams");
    if (nb <= 0) nb = 0;

    // Capture the producer's current selection BEFORE we iterate so
    // we can mark which streams are defaults. Reading video_index /
    // audio_index from the producer (not the caller's request) is what
    // matters: if the caller passed -1, MLT picks the first of each
    // type, and we want the actual picked index.
    int cur_v = producer.get_int("video_index");
    int cur_a = producer.get_int("audio_index");
    // audio_index = "all" is encoded as INT_MAX inside MLT; the int
    // getter returns that. We surface -2 to the caller (our sentinel),
    // since "INT_MAX" leaks an implementation detail. Detect "all"
    // by checking the raw string.
    const char* a_raw = props.get("audio_index");
    bool audio_all = a_raw && std::strcmp(a_raw, "all") == 0;

    item.video_index = cur_v;
    item.audio_index = audio_all ? -2 : cur_a;

    for (int i = 0; i < nb; ++i) {
        char key[128];
        std::snprintf(key, sizeof(key), "meta.media.%d.stream.type", i);
        const char* type_str = props.get(key);

        std::snprintf(key, sizeof(key), "meta.media.%d.codec.name", i);
        const char* codec = props.get(key);

        std::snprintf(key, sizeof(key), "meta.media.%d.codec.long_name", i);
        const char* long_name = props.get(key);

        std::snprintf(key, sizeof(key), "meta.attr.%d.stream.language.markup", i);
        const char* lang = props.get(key);

        StreamInfo s;
        s.index = i;
        s.codec = codec ? codec : "";
        s.long_name = long_name ? long_name : "";
        s.language = lang ? lang : "";

        if (type_str) {
            s.type = type_str;  // "video" or "audio"
        } else if (looks_like_subtitle(s.long_name) || looks_like_subtitle(s.codec)) {
            s.type = "subtitle";
        } else {
            // Unknown stream type (data / attachment) — skip. We do
            // not surface streams we cannot describe.
            continue;
        }

        if (s.type == "video") {
            std::snprintf(key, sizeof(key), "meta.media.%d.codec.width", i);
            s.width = props.get_int(key);
            std::snprintf(key, sizeof(key), "meta.media.%d.codec.height", i);
            s.height = props.get_int(key);
            std::snprintf(key, sizeof(key), "meta.media.%d.stream.frame_rate", i);
            s.frame_rate = props.get_double(key);
            s.is_default = (i == cur_v);
        } else if (s.type == "audio") {
            std::snprintf(key, sizeof(key), "meta.media.%d.codec.channels", i);
            s.channels = props.get_int(key);
            std::snprintf(key, sizeof(key), "meta.media.%d.codec.sample_rate", i);
            s.sample_rate = props.get_int(key);
            s.is_default = audio_all || (i == cur_a);
        }
        // subtitle: no extra fields in this iteration; consumers read
        // codec/long_name/language.

        item.streams.push_back(std::move(s));
    }
}

int MediaLibrary::import(const std::string& path, int video_index, int audio_index) {
    auto found = by_path_.find(path);
    if (found != by_path_.end()) return found->second;

    auto producer = std::make_unique<Mlt::Producer>(profile_, path.c_str());
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Error, "MediaLibrary: failed to open");
        return -1;
    }

    // Apply the caller's stream selection before probe_streams so the
    // producer reflects the chosen video stream's properties.
    apply_stream_selection(*producer, video_index, audio_index);

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

    // Populate the stream list and record the producer's chosen indices.
    probe_streams(item, *producer);

    int index = static_cast<int>(items_.size());
    items_.push_back(std::move(item));
    producers_.push_back(std::move(producer));
    by_path_[path] = index;
    return index;
}

bool MediaLibrary::set_stream(int index, int video_index, int audio_index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    if (!producers_[index] || !producers_[index]->is_valid()) return false;

    // We cannot mutate the existing producer's audio_index / video_index
    // after it has decoded a frame: MLT's producer_avformat caches the
    // chosen stream's decoder context, and a subsequent get_frame() will
    // silently reset the property back to the cached stream's index.
    // (Verified empirically — see lessons #14.) Re-opening the producer
    // is the only way to get a clean decoder. The path is preserved
    // (already-imported sources keep their library index), but the new
    // producer shares nothing with the old one beyond the path.
    std::string path = items_[index].path;

    auto next = std::make_unique<Mlt::Producer>(profile_, path.c_str());
    if (!next || !next->is_valid()) {
        Logger::log(Logger::Level::Error, "MediaLibrary::set_stream: reopen failed");
        return false;
    }

    // Apply the new selection BEFORE the first get_frame() so the decoder
    // initializes on the right stream from the start.
    apply_stream_selection(*next, video_index, audio_index);

    // Swap in the new producer; the old one (and any clips cut from it)
    // is dropped. Caller guarantees (via AppController::library_set_stream
    // refusing in-use sources) there are no live clips referencing this
    // producer, so swapping is safe.
    producers_[index] = std::move(next);

    // Re-probe the item's metadata from the new producer.
    MediaItem& item = items_[index];
    std::string saved_path = item.path;
    std::string saved_name = item.name;
    item = MediaItem{};
    item.path = saved_path;
    item.name = saved_name;
    item.frame_count = producers_[index]->get_length();
    item.has_video = producers_[index]->get_int("video_index") >= 0;
    item.has_audio = producers_[index]->get_int("audio_index") >= 0;
    if (item.has_video) {
        Mlt::Frame* frame = producers_[index]->get_frame();
        if (frame && frame->is_valid()) {
            int w = frame->get_int("meta.media.width");
            int h = frame->get_int("meta.media.height");
            if (w <= 0) w = producers_[index]->get_int("meta.media.width");
            if (h <= 0) h = producers_[index]->get_int("meta.media.height");
            item.width = w;
            item.height = h;
            delete frame;
        }
        int fps_num = producers_[index]->get_int("meta.media.frame_rate_num");
        int fps_den = producers_[index]->get_int("meta.media.frame_rate_den");
        item.fps = fps_from_rational(fps_num, fps_den);
        if (item.fps <= 0 && item.frame_count > 0) {
            item.fps = static_cast<double>(producers_[index]->get_double("fps"));
        }
    }
    if (item.fps > 0 && item.frame_count > 0) {
        item.duration_sec = item.frame_count / item.fps;
    }
    probe_streams(item, *producers_[index]);
    return true;
}

bool MediaLibrary::remove(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    // Drop the path entry first so a re-import would mint a new index.
    by_path_.erase(items_[index].path);
    items_.erase(items_.begin() + index);
    producers_.erase(producers_.begin() + index);
    // Re-key the by_path_ index map for the shifted tail.
    for (int i = index; i < static_cast<int>(items_.size()); ++i) {
        by_path_[items_[i].path] = i;
    }
    return true;
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
