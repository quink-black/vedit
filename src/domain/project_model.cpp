#include "project_model.h"

namespace vedit {

ProjectModel::ProjectModel()
    : library_(std::make_unique<MediaLibrary>(profile_)),
      timeline_(std::make_unique<TimelineModel>(profile_, *library_)) {}

ProjectModel::~ProjectModel() = default;

} // namespace vedit
