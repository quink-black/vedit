#include "project_model.h"

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

} // namespace vedit
