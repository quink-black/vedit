#include "project_model.h"

namespace vedit {

struct ProjectModel::Impl {};
ProjectModel::ProjectModel() : impl_(std::make_unique<Impl>()) {}
ProjectModel::~ProjectModel() = default;

} // namespace vedit
