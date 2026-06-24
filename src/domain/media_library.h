#ifndef VEDIT_DOMAIN_MEDIA_LIBRARY_H
#define VEDIT_DOMAIN_MEDIA_LIBRARY_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mlt.h>

namespace vedit {

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
    // index.
    int import(const std::string& path);

    int count() const { return static_cast<int>(items_.size()); }
    const MediaItem* item(int index) const;
    Mlt::Producer* producer(int index);

private:
    Mlt::Profile& profile_;
    std::vector<std::unique_ptr<Mlt::Producer>> producers_;
    std::vector<MediaItem> items_;
    std::unordered_map<std::string, int> by_path_;
};

} // namespace vedit

#endif
