#ifndef VEDIT_DOMAIN_MEDIA_LIBRARY_H
#define VEDIT_DOMAIN_MEDIA_LIBRARY_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mlt.h>

namespace vedit {

// Description of one stream in a container (video / audio / subtitle).
// Populated from MLT's meta.media.<i>.* properties at import time.
// subtitle streams: MLT's producer_avformat does not write
// `stream.type` for non-video/audio streams (its switch default is
// empty), so subtitle detection falls back to matching "subtitle" in
// `codec.long_name` (e.g. "Timed text subtitle" / "ASS subtitle").
struct StreamInfo {
    int index = -1;             // container stream index
    std::string type;           // "video" | "audio" | "subtitle"
    std::string codec;          // meta.media.<i>.codec.name
    std::string long_name;      // meta.media.<i>.codec.long_name (may be empty)
    std::string language;       // meta.attr.<i>.stream.language.markup (may be empty)
    // video-only:
    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    // audio-only:
    int channels = 0;
    int sample_rate = 0;
    bool is_default = false;    // matches the producer's current video_index/audio_index
};

// One imported source file plus its probed metadata.
struct MediaItem {
    std::string path;
    std::string name;          // basename
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t frame_count = 0;   // in source frames
    double duration_sec = 0.0;
    bool has_audio = false;
    bool has_video = false;

    // All streams in the container (video, audio, subtitle). Empty
    // until import() probes them.
    std::vector<StreamInfo> streams;
    // Producer's currently-selected stream indices. -1 means "none
    // of that type" (e.g. an audio-only file has video_index=-1).
    int video_index = -1;
    int audio_index = -1;
};

// Owns the Mlt::Producer for each imported source. Producers are shared
// with the timeline (a clip is a cut of a library producer), so the
// library must outlive any clip that references it.
class MediaLibrary {
public:
    MediaLibrary(Mlt::Profile& profile);
    ~MediaLibrary();

    // Import a file. Returns the item index, or -1 on failure.
    // Re-importing the same path is a no-op and returns the existing
    // index (the existing stream selection is preserved; new
    // video_index/audio_index are ignored on re-import).
    //
    // video_index / audio_index: optional stream selection. Pass -1
    // (default) to accept MLT's first-of-type. Pass >= 0 to select a
    // specific stream. audio_index == -2 selects "all" audio streams
    // (decodes every audio stream interleaved — see producer_avformat).
    int import(const std::string& path,
               int video_index = -1, int audio_index = -1);

    // Switch the selected video/audio stream on an already-imported
    // item. Refuses (returns false) if `index` is out of range or the
    // caller has not verified the timeline holds no clips referencing
    // this index (caller responsibility — see
    // TimelineModel::count_clips_for_media). Stream indices follow the
    // same convention as `import` (-1 = none, -2 audio = all).
    bool set_stream(int index, int video_index, int audio_index);

    // Remove a library item by index. Refuses and returns false if the
    // caller has not verified the timeline holds no clips referencing
    // this index (caller responsibility — see
    // TimelineModel::count_clips_for_media). After removal, indices
    // above `index` shift down by one to keep the vector dense.
    bool remove(int index);

    int count() const { return static_cast<int>(items_.size()); }
    const MediaItem* item(int index) const;
    Mlt::Producer* producer(int index);

private:
    // Probe the producer's meta.media.* properties and populate
    // `item.streams` / `item.video_index` / `item.audio_index`. Called
    // once at the end of import().
    void probe_streams(MediaItem& item, Mlt::Producer& producer);

    Mlt::Profile& profile_;
    std::vector<std::unique_ptr<Mlt::Producer>> producers_;
    std::vector<MediaItem> items_;
    std::unordered_map<std::string, int> by_path_;
};

} // namespace vedit

#endif
