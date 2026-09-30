#include "project_save_load.h"

#include "media_library.h"
#include "project_model.h"
#include "timeline_model.h"

#include <algorithm>
#include <filesystem>
#include <vector>
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
nlohmann::json sidecar_for(const TimelineModel& tl) {
    nlohmann::json entries = nlohmann::json::array();
    nlohmann::json track_ids = nlohmann::json::array();
    const auto tracks = tl.tracks();
    for (size_t t = 0; t < tracks.size(); ++t) {
        track_ids.push_back(tracks[t].id.value);
        const auto clips = tl.clips_on_track(tracks[t].id);
        for (size_t c = 0; c < clips.size(); ++c) {
            nlohmann::json entry;
            entry["track"] = t;
            entry["clip"] = c;
            entry["clip_id"] = clips[c].id.value;
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
    doc["track_ids"] = track_ids;
    // The counters go with them: a project restored from this document is
    // smaller than the one it replaces, and an id that was spent before the
    // save must not be handed out again afterwards.
    doc["next_clip_id"] = tl.id_watermark().next_clip;
    doc["next_track_id"] = tl.id_watermark().next_track;

    // MLT keeps the mix across a round trip but not the type that was asked
    // for, so the transitions are recorded by position.
    nlohmann::json transitions = nlohmann::json::array();
    for (const auto& u : tl.transitions()) {
        for (size_t t = 0; t < tracks.size(); ++t) {
            if (!(tracks[t].id == u.track_id)) continue;
            transitions.push_back({{"track", t},
                                   {"index", u.clip_index},
                                   {"type", u.type}});
        }
    }
    doc["transitions"] = transitions;
    return doc;
}

// Adopt mints ids from one upwards in document order, which is not the
// numbering the saved document carried: every handle handed out before the
// save or the undo would address the wrong object, or nothing at all.
// Ids are restored before filters are re-attached, because filter
// addressing goes through them.
void restore_ids(const nlohmann::json& doc, TimelineModel& tl) {
    // Park first, then move into place. Two clips that trade ids would each
    // find the other's id already taken, so every id that changes hands is
    // first lifted above the watermark, where no live object can be.
    auto reassign = [&](std::vector<std::pair<int64_t, int64_t>>& pending,
                        int64_t saved_watermark, bool clip) {
        int64_t scratch = std::max(saved_watermark, clip ? tl.id_watermark().next_clip
                                                    : tl.id_watermark().next_track);
        std::vector<std::pair<int64_t, int64_t>> parked;
        for (const auto& [current, wanted] : pending) {
            if (current == wanted) continue;
            int64_t temp = ++scratch;
            bool ok = clip ? tl.assign_clip_id(ClipId{current}, ClipId{temp})
                           : tl.assign_track_id(TrackId{current}, TrackId{temp});
            if (ok) parked.push_back({temp, wanted});
        }
        for (const auto& [temp, wanted] : parked) {
            if (clip) tl.assign_clip_id(ClipId{temp}, ClipId{wanted});
            else tl.assign_track_id(TrackId{temp}, TrackId{wanted});
        }
    };

    std::vector<std::pair<int64_t, int64_t>> tracks_pending;
    if (doc.contains("track_ids")) {
        const auto tracks = tl.tracks();
        const auto& ids = doc["track_ids"];
        for (size_t t = 0; t < tracks.size() && t < ids.size(); ++t) {
            int64_t wanted = ids[t].get<int64_t>();
            if (wanted > 0) tracks_pending.push_back({tracks[t].id.value, wanted});
        }
    }

    std::vector<std::pair<int64_t, int64_t>> clips_pending;
    if (doc.contains("clips")) {
        for (const auto& entry : doc["clips"]) {
            int t = entry.value("track", -1);
            int c = entry.value("clip", -1);
            int64_t wanted = entry.value("clip_id", (int64_t)0);
            if (t < 0 || c < 0 || wanted <= 0) continue;
            const auto tracks = tl.tracks();
            if (t >= static_cast<int>(tracks.size())) continue;
            const auto clips = tl.clips_on_track(tracks[t].id);
            if (c >= static_cast<int>(clips.size())) continue;
            clips_pending.push_back({clips[c].id.value, wanted});
        }
    }

    reassign(tracks_pending, doc.value("next_track_id", (int64_t)0), false);
    reassign(clips_pending, doc.value("next_clip_id", (int64_t)0), true);

    TimelineModel::IdWatermark saved;
    saved.next_clip = doc.value("next_clip_id", (int64_t)0);
    saved.next_track = doc.value("next_track_id", (int64_t)0);
    tl.reserve_ids(saved);
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
    restore_ids(doc, tl);

    // Re-mark the restored mixes with the type they were planted with.
    if (doc.contains("transitions")) {
        const auto restored_tracks = tl.tracks();
        for (const auto& entry : doc["transitions"]) {
            int t = entry.value("track", -1);
            int index = entry.value("index", -1);
            std::string type = entry.value("type", "");
            if (t < 0 || index < 0 || type.empty()) continue;
            if (t >= static_cast<int>(restored_tracks.size())) continue;
            tl.set_transition_type(restored_tracks[t].id, index, type);
        }
    }

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

std::string ProjectSaveLoad::sidecar_path(const std::string& xml_path) {
    return xml_path + ".vedit";
}

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
    if (!sidecar["clips"].empty() || !sidecar["track_ids"].empty()) {
        std::ofstream out(sidecar_path(normalized));
        if (out) out << sidecar.dump(2);
    } else {
        // Nothing to carry now, but a sidecar left from an earlier save to
        // the same path would re-attach filters and ids this project does
        // not have.
        std::error_code ec;
        std::filesystem::remove(sidecar_path(normalized), ec);
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
    TimelineModel& tl = project.timeline();
    MediaLibrary& lib = project.library();

    // Built with nlohmann-json (not hand-rolled) so the schema is
    // exact and escaping is correct. The shape is the contract an agent
    // reads to discover media, streams, tracks, clips and transitions.
    // Add fields freely; renaming or removing one needs the callers
    // changed in the same commit, which is how filters.available and
    // export_presets left this view for filter.list and
    // export.list_presets.
    nlohmann::json doc;
    doc["project"] = "vedit";

    // Changes and history steps move this. A client that read the state
    // passes it back as expected_revision to undo/redo, which is how a
    // second client learns its view is stale instead of overwriting work.
    doc["revision"] = project.revision();

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
            cj["playlist_index"] = c.playlist_index;
            clips.push_back(std::move(cj));
        }
        tj["clips"] = std::move(clips);
        tracks.push_back(std::move(tj));
    }
    doc["tracks"] = std::move(tracks);

    // Transitions occupy a playlist slot of their own but are not clips,
    // so they get their own list. clip_index is the slot the mix sits in,
    // which is what transition.add reports as clip_index + 1 for its right
    // neighbour. The type is whatever was requested at planting time; a
    // project saved before the tag existed reports an empty string.
    nlohmann::json transitions = nlohmann::json::array();
    for (const auto& tr : tl.transitions()) {
        nlohmann::json uj;
        uj["track_id"] = tr.track_id.value;
        uj["clip_index"] = tr.clip_index;
        uj["length"] = tr.length;
        uj["type"] = tr.type;
        transitions.push_back(std::move(uj));
    }
    doc["transitions"] = std::move(transitions);

    // The filters in place on clips and tracks. What can be attached is a
    // question for filter.list, and which presets exist is a question for
    // export.list_presets; neither belongs in an observation of the project.
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

    doc["filters"] = nlohmann::json{{"attached", std::move(attached)}};

    return doc.dump();
}

} // namespace vedit
