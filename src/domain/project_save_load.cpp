#include "project_save_load.h"

#include "media_library.h"
#include "project_model.h"
#include "timeline_model.h"
#include "util/logger.h"

#include <cstdio>
#include <sstream>

// MLT C API: MLT_TRACTOR cast for track enumeration (no mlt++ wrapper).
#include <framework/mlt_tractor.h>

namespace vedit {

bool ProjectSaveLoad::save_xml(ProjectModel& project, const std::string& path) {
    Mlt::Producer* producer = project.producer();
    if (!producer || !producer->is_valid()) {
        Logger::log(Logger::Level::Error, "ProjectSaveLoad: no producer to save");
        return false;
    }

    Mlt::Consumer consumer(project.profile(), "xml", path.c_str());
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
    // Deserialize the XML into a temporary producer.
    Mlt::Producer loaded(project.profile(), "xml", path.c_str());
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
    TimelineModel& tl = project.timeline();
    MediaLibrary& lib = project.library();

    std::ostringstream ss;
    ss << "{\"tracks\":[";
    auto tracks = tl.tracks();
    for (size_t t = 0; t < tracks.size(); ++t) {
        if (t > 0) ss << ",";
        ss << "{\"name\":\"" << tracks[t].name << "\",";
        ss << "\"kind\":\"" << (tracks[t].kind == TrackKind::Video ? "video" : "audio") << "\",";
        ss << "\"clips\":[";
        auto clips = tl.clips_on_track(tracks[t].id);
        for (size_t c = 0; c < clips.size(); ++c) {
            if (c > 0) ss << ",";
            ss << "{\"start\":" << clips[c].start << ",";
            ss << "\"length\":" << clips[c].length << ",";
            ss << "\"in\":" << clips[c].in_point << ",";
            ss << "\"out\":" << clips[c].out_point << ",";
            const auto* item = lib.item(clips[c].media_index);
            ss << "\"media\":\"" << (item ? item->path : "") << "\"";
            ss << "}";
        }
        ss << "]}";
    }
    ss << "]}";
    return ss.str();
}

} // namespace vedit
