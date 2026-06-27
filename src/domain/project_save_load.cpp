#include "project_save_load.h"

#include "export_service.h"
#include "filter_catalog.h"
#include "media_library.h"
#include "project_model.h"
#include "timeline_model.h"

#include <algorithm>
#include "util/logger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <sstream>

// MLT C API: MLT_TRACTOR cast for track enumeration (no mlt++ wrapper).
#include <framework/mlt_tractor.h>

namespace vedit {

namespace {

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
    consumer.set("resource", path.c_str());
    consumer.set("no_root", 0);
    consumer.connect(*producer);
    consumer.run();

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

    TimelineModel& tl = project.timeline();
    MediaLibrary& lib = project.library();

    // Remove all existing tracks first.
    while (tl.track_count() > 0) {
        tl.remove_track(tl.tracks()[0].id);
    }

    // Enumerate tracks via the C API (no mlt++ wrapper for tractor
    // track enumeration). The XML-loaded producer is a tractor; each
    // track is a playlist producer.
    int num_tracks = 0;
    while (mlt_producer producer = mlt_tractor_get_track(
               MLT_TRACTOR(loaded.get_producer()), num_tracks)) {
        ++num_tracks;
    }
    // Reset — we consumed the tracks above.
    loaded = Mlt::Producer(project.profile(), "xml", path.c_str());
    if (!loaded.is_valid()) return false;

    for (int t = 0; t < num_tracks; ++t) {
        mlt_producer raw = mlt_tractor_get_track(
            MLT_TRACTOR(loaded.get_producer()), t);
        if (!raw) continue;

        Mlt::Producer track_prod(raw);
        Mlt::Playlist pl(track_prod);

        // Infer track kind from clip content (M1 heuristic: if any
        // clip has video, it's a video track; else audio).
        TrackKind kind = TrackKind::Video;
        for (int c = 0; c < pl.count(); ++c) {
            if (pl.is_blank(c)) continue;
            Mlt::Producer* clip = pl.get_clip(c);
            if (clip && clip->get_int("video_index") >= 0) {
                kind = TrackKind::Video;
                delete clip;
                break;
            }
            delete clip;
            kind = TrackKind::Audio;
        }

        auto track_id = tl.add_track(kind);
        if (!track_id.valid()) continue;

        for (int c = 0; c < pl.count(); ++c) {
            Mlt::ClipInfo* ci = pl.clip_info(c);
            if (!ci) continue;

            std::string source = ci->resource ? ci->resource : "";
            if (source.empty() || pl.is_blank(c)) {
                Mlt::Playlist::delete_clip_info(ci);
                continue;
            }

            int media_idx = lib.import(source);
            if (media_idx < 0) {
                Mlt::Playlist::delete_clip_info(ci);
                continue;
            }

            tl.append_clip(track_id, media_idx, ci->frame_in, ci->frame_out);
            Mlt::Playlist::delete_clip_info(ci);
        }
    }

    tl.replant_compositions();
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
