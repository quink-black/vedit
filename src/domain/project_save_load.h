#ifndef VEDIT_DOMAIN_PROJECT_SAVE_LOAD_H
#define VEDIT_DOMAIN_PROJECT_SAVE_LOAD_H

#include <string>

#include <Mlt.h>

namespace vedit {

class ProjectModel;
class TimelineModel;
class FilterCatalog;

// Serializes a ProjectModel to/from MLT XML (consumer_xml / producer_xml).
// The JSON projection (state_json) is a derived view of the timeline for
// agent consumption — agents never parse MLT XML.
//
// Threading: synchronous; call from the main thread.
class ProjectSaveLoad {
public:
    // Save the project's tractor to an MLT XML file. Returns true on
    // success. The XML includes all tracks, clips, and transitions.
    static bool save_xml(ProjectModel& project, const std::string& path);

    // Load an MLT XML file into an existing ProjectModel, replacing what is
    // open: the parsed graph is adopted in place of the current one, and the
    // timeline keeps the ids the file carried. Returns true on success; a
    // file that cannot be parsed leaves the open project untouched.
    static bool load_xml(ProjectModel& project, const std::string& path);

    // Path of the file that travels beside the XML and carries what the
    // document cannot: per-clip filters with their parameters and keyframes,
    // and the ids the timeline held. Anything that deletes a saved project
    // or snapshot deletes this alongside it.
    static std::string sidecar_path(const std::string& xml_path);

    // Build a JSON string describing the current timeline state.
    // Format: {"tracks": [{"name": "...", "kind": "video"|"audio",
    //   "clips": [{"start": 0, "length": 125, "in": 0, "out": 124,
    //              "media": "path/to/file.mp4"}]}]}
    // This is the state.get projection for agents.
    static std::string state_json(ProjectModel& project);
    // Overload with filter catalog — populates filters.available from
    // the catalog and filters.attached from the timeline.
    static std::string state_json(ProjectModel& project,
                                   const FilterCatalog& catalog);
};

} // namespace vedit

#endif
