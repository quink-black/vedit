#ifndef VEDIT_DOMAIN_PROJECT_MODEL_H
#define VEDIT_DOMAIN_PROJECT_MODEL_H

#include <memory>

namespace vedit {

// Skeleton. The real MLT-backed tractor/playlist/multitrack land in T4.
// For T2 it exists only so the layer graph compiles end-to-end.
class ProjectModel {
public:
    ProjectModel();
    ~ProjectModel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vedit

#endif
