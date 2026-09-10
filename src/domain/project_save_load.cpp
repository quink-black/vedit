#include "project_save_load.h"

#include "export_service.h"
#include "filter_catalog.h"
#include "media_library.h"
#include "project_model.h"
#include "timeline_model.h"

#include <algorithm>
#include "util/logger.h"

#include <nlohmann/json.hpp>

#include <fstream>

#include <cstdio>
#include <sstream>

// MLT C API: MLT_TRACTOR cast for track enumeration (no mlt++ wrapper).
#include <framework/mlt_tractor.h>

namespace vedit {

namespace {

// MLT's XML reader attaches a filter written inside a playlist entry to
// the entry's parent producer rather than to the cut, so per-clip
// filters migrate to the shared source producer across a round trip.
// They are recorded beside the document, keyed by track and clip index
// (document order, which adopt preserves), and re-attached on load.
std::string sidecar_path(const std::string& xml_path) {
    return xml_path + ".vedit";
}

nlohmann::json sidecar_for(const TimelineModel& tl) {
    nlohmann::json entries = nlohmann::json::array();
    const auto tracks = tl.tracks();
    for (size_t t = 0; t < tracks.size(); ++t) {
        const auto clips = tl.clips_on_track(tracks[t].id);
        for (size_t c = 0; c < clips.size(); ++c) {
            nlohmann::json entry;
            entry["track"] = t;
            entry["clip"] = c;
            nlohmann::json filters = nlohmann::json::array();
            for (const auto& f : tl.attached_filters()) {
                if (f.target_type != "clip" || f.target_id != clips[c].id.value)
                    continue;
                nlohmann::json fj;
                fj["name"] = f.filter_name;
                nlohmann::json params = nlohmann::json::array();
                for (const auto& [key, value] : f.params) {
                    params.push_back({{"key", key}, {"value", value}});
                }
                fj["params"] = params;
                filters.push_back(fj);
            }
            entry["filters"] = filters;
            entries.push_back(entry);
        }
    }
    nlohmann::json doc;
    doc["vedit_sidecar"] = 1;
    doc["clips"] = entries;
    return doc;
}

void apply_sidecar(const std::string& path, TimelineModel& tl) {
    std::ifstream in(path);
    if (!in) return;
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception&) {
        Logger::log(Logger::Level::Warn,
                    "ProjectSaveLoad: unreadable sidecar, filters skipped");
        return;
    }
    if (!doc.contains("clips")) return;
    const auto tracks = tl.tracks();
    for (const auto& entry : doc["clips"]) {
        int t = entry.value("track", -1);
        int c = entry.value("clip", -1);
        if (t < 0 || c < 0 || t >= static_cast<int>(tracks.size())) continue;
        const auto clips = tl.clips_on_track(tracks[t].id);
        if (c >= static_cast<int>(clips.size())) continue;
        const ClipId clip_id = clips[c].id;

        // The sidecar is authoritative: MLT already attached the
        // document's entry filters, and attaching again would double
        // them up.
        for (const auto& f : tl.attached_filters()) {
            if (f.target_type == "clip" && f.target_id == clip_id.value)
                tl.detach_filter(clip_id, f.filter_name);
        }

        for (const auto& f : entry.value("filters", nlohmann::json::array())) {
            std::string name = f.value("name", "");
            if (name.empty()) continue;
            int in = f.value("in", -1);
            int out = f.value("out", -1);
            if (!tl.attach_filter(clip_id, name, in, out)) continue;
            for (const auto& pv : f.value("params", nlohmann::json::array())) {
                std::string key = pv.value("key", "");
                if (key.empty()) continue;
                const nlohmann::json& v = pv["value"];
                if (v.contains("keyframes")) {
                    tl.set_filter_param_keyframes(clip_id, name, key,
                                                  v["keyframes"]);
                } else if (v.contains("value")) {
                    const nlohmann::json& scalar = v["value"];
                    std::string text = scalar.is_string()
                                           ? scalar.get<std::string>()
                                           : scalar.dump();
                    tl.set_filter_param(clip_id, name, key, text);
                }
            }
        }
    }
}

} // namespace

bool ProjectSaveLoad::save_xml(ProjectModel& project, const std::string& path) {
    Mlt::Producer* producer = project.producer();
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Error, "ProjectSaveLoad: no producer to save");
        return false;
    }

    // Normalize to forward slashes: MLT's consumer_xml path parsing and
    // the subsequent load_xml (producer_xml) work with forward slashes
    // on all platforms. Windows backslashes can confuse the XML resource
    // path resolution.
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    Mlt::Consumer consumer(project.profile(), "xml", normalized.c_str());
    if (!consumer.is_valid()) {
        Logger::log(Logger::Level::Error, "ProjectSaveLoad: xml consumer unavailable");
        return false;
    }
    consumer.set("resource", normalized.c_str());
    consumer.set("no_root", 0);
    consumer.connect(*producer);
    consumer.run();

    // Record what MLT cannot carry, then clean up stale sidecars.
    std::remove(sidecar_path(normalized).c_str());
    nlohmann::json sidecar = sidecar_for(project.timeline());
    if (!sidecar["clips"].empty()) {
        std::ofstream out(sidecar_path(normalized));
        if (out) out << sidecar.dump(2);
    }

    Logger::log(Logger::Level::Info, "ProjectSaveLoad: saved XML");
    return true;
}

bool ProjectSaveLoad::load_xml(ProjectModel& project, const std::string& path) {
    // Normalize to forward slashes (see save_xml above).
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Deserialize the XML into a temporary producer.
    Mlt::Producer loaded(project.profile(), "xml", normalized.c_str());
    if (!loaded.is_valid()) {
        Logger::log(Logger::Level::Error, "ProjectSaveLoad: invalid XML file");
        return false;
    }

    // Adopt the loaded graph as-is. Rebuilding the tracks from the
    // library instead kept only clip in/out points and dropped every
    // filter, transition, keyframe, per-clip effect and blank.
    if (!project.timeline().adopt(loaded)) {
        Logger::log(Logger::Level::Error,
                    "ProjectSaveLoad: XML does not contain a tractor");
        return false;
    }

    apply_sidecar(sidecar_path(normalized), project.timeline());

    Logger::log(Logger::Level::Info, "ProjectSaveLoad: loaded XML");
    return true;
}

std::string ProjectSaveLoad::state_json(ProjectModel& project) {
    // Default: no catalog. filters.available will be empty.
    return state_json(project, FilterCatalog{});
}

std::string ProjectSaveLoad::state_json(ProjectModel& project,
                                         const FilterCatalog& catalog) {
    TimelineModel& tl = project.timeline();
    MediaLibrary& lib = project.library();

    // Built with nlohmann-json (not hand-rolled) so the schema is
    // exact and escaping is correct. The shape is the stable contract
    // an agent reads to discover media, streams, tracks, and clips —
    // changing it breaks MCP consumers. Add fields, do not rename or
    // remove existing ones.
    nlohmann::json doc;
    doc["project"] = "vedit";

    // Profile (canvas dimensions). Agent needs this to know the
    // current output resolution before positioning clips.
    doc["profile"] = nlohmann::json{
        {"width",  project.profile().width()},
        {"height", project.profile().height()},
        {"fps",    project.profile().fps()},
        {"progressive", 1}
    };

    // Library: list every imported source with its probed metadata and
    // the full stream list (video / audio / subtitle). The agent uses
    // this to choose a media_index for clip.append/insert, pick which
    // stream to import via video_index/audio_index, and detect
    // subtitle streams it might want to render via a text filter later.
    nlohmann::json library = nlohmann::json::array();
    for (int i = 0; i < lib.count(); ++i) {
        const auto* item = lib.item(i);
        if (!item) continue;
        nlohmann::json j;
        j["index"] = i;
        {
            // Normalize to forward slashes so JSON consumers on all
            // platforms see a consistent path format (Windows backslash
            // paths break string comparisons and URL-path assumptions).
            std::string p = item->path;
            std::replace(p.begin(), p.end(), '\\', '/');
            j["path"] = std::move(p);
        }
        j["name"] = item->name;
        j["width"] = item->width;
        j["height"] = item->height;
        j["fps"] = item->fps;
        j["duration_sec"] = item->duration_sec;
        j["frame_count"] = item->frame_count;
        j["has_video"] = item->has_video;
        j["has_audio"] = item->has_audio;
        j["video_index"] = item->video_index;
        j["audio_index"] = item->audio_index;

        nlohmann::json streams = nlohmann::json::array();
        for (const auto& s : item->streams) {
            nlohmann::json sj;
            sj["index"] = s.index;
            sj["type"] = s.type;
            sj["codec"] = s.codec;
            sj["long_name"] = s.long_name;
            sj["language"] = s.language;
            sj["is_default"] = s.is_default;
            if (s.type == "video") {
                sj["width"] = s.width;
                sj["height"] = s.height;
                sj["frame_rate"] = s.frame_rate;
            } else if (s.type == "audio") {
                sj["channels"] = s.channels;
                sj["sample_rate"] = s.sample_rate;
            }
            // subtitle: only index/type/codec/long_name/language/is_default.
            streams.push_back(std::move(sj));
        }
        j["streams"] = std::move(streams);
        library.push_back(std::move(j));
    }
    doc["library"] = std::move(library);

    // Tracks with their clips. Each clip carries its stable ClipId and
    // media_index so the agent can target it via clip.remove/move/trim.
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& tr : tl.tracks()) {
        nlohmann::json tj;
        tj["id"] = tr.id.value;
        tj["name"] = tr.name;
        tj["kind"] = (tr.kind == TrackKind::Video) ? "video" : "audio";
        tj["muted"] = tr.muted;
        tj["hidden"] = tr.hidden;
        nlohmann::json clips = nlohmann::json::array();
        for (const auto& c : tl.clips_on_track(tr.id)) {
            nlohmann::json cj;
            cj["id"] = c.id.value;
            cj["media_index"] = c.media_index;
            cj["start"] = c.start;
            cj["length"] = c.length;
            cj["in"] = c.in_point;
            cj["out"] = c.out_point;
            clips.push_back(std::move(cj));
        }
        tj["clips"] = std::move(clips);
        tracks.push_back(std::move(tj));
    }
    doc["tracks"] = std::move(tracks);

    // ---- Filters (M2: populated) ----
    //
    // P1 (generic, yml-driven): available from MLT repository.
    // P2 (keyframe-aware): params accept {value}|{keyframes}.
    // attached lists all filters currently on clips/tracks.
    nlohmann::json available = nlohmann::json::array();
    for (const auto& f : catalog.filters()) {
        nlohmann::json fj;
        fj["id"] = f.id;
        fj["title"] = f.title;
        fj["is_audio"] = f.is_audio;
        fj["is_video"] = f.is_video;
        nlohmann::json fparams = nlohmann::json::array();
        for (const auto& p : f.params) {
            nlohmann::json pj;
            pj["identifier"] = p.identifier;
            pj["type"] = p.type;
            pj["default"] = p.default_value;
            if (p.animation) pj["animation"] = true;
            fparams.push_back(std::move(pj));
        }
        fj["params"] = std::move(fparams);
        available.push_back(std::move(fj));
    }

    nlohmann::json attached = nlohmann::json::array();
    for (const auto& af : tl.attached_filters()) {
        nlohmann::json afj;
        afj["target_type"] = af.target_type;
        afj["target_id"] = af.target_id;
        afj["filter"] = af.filter_name;
        nlohmann::json af_params = nlohmann::json::object();
        for (const auto& [k, v] : af.params) {
            af_params[k] = v;
        }
        afj["params"] = std::move(af_params);
        attached.push_back(std::move(afj));
    }

    doc["filters"] = nlohmann::json{
        {"available", std::move(available)},
        {"attached",  std::move(attached)}
    };

    // Export presets.
    {
        auto presets = ExportService::list_presets();
        nlohmann::json export_presets = nlohmann::json::array();
        for (const auto& p : presets) {
            nlohmann::json pj;
            pj["name"] = p.name;
            pj["extension"] = p.extension;
            pj["description"] = p.description;
            export_presets.push_back(std::move(pj));
        }
        doc["export_presets"] = std::move(export_presets);
    }

    return doc.dump();
}

} // namespace vedit
