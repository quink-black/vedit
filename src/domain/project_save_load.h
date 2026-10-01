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
    // open: the parsed graph is adopted in place of the current one, the media
    // library is rebuilt from what the file carries, and the timeline keeps the
    // ids the file held. Returns true on success; a file that cannot be parsed
    // leaves the open project untouched, library included.
    static bool load_xml(ProjectModel& project, const std::string& path);

    // Restore a state whose document is empty because the project had no
    // tracks: the timeline is emptied and the library is rebuilt from the record
    // beside the document, which is the only place the sources imported at that
    // point are written down.
    static void restore_empty(ProjectModel& project, const std::string& path);

    // Path of the file that travels beside the XML and carries what the
    // document cannot: per-clip filters with their parameters and keyframes,
    // the ids the timeline held, and the media the project had imported. The
    // record states the size of the document it was written for, and a load
    // that finds a different document beside it skips the record rather than
    // grafting another write's entries onto this project. Anything that deletes
    // a saved project or snapshot deletes this alongside it.
    static std::string sidecar_path(const std::string& xml_path);

    // Build the state.get projection: the project as an agent sees it.
    // Format: {"profile": ..., "library": [...], "tracks": [...],
    // "transitions": [...], "filters": {"attached": [...]}}.
    //
    // Two catalogues used to ride along: the filter list, which runs to
    // about 44 kB and never changes while editing, and the export preset
    // list. They live in filter.list and export.list_presets now, because an
    // observation of the timeline should cost what the timeline costs.
    static std::string state_json(ProjectModel& project);
};

} // namespace vedit

#endif
