#include "rpc_methods.h"

#include "rpc_dispatcher.h"
#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/project_save_load.h"

#include <unistd.h>

namespace vedit::rpc {

void register_methods(Dispatcher& d, AppController& app) {
    // ---- app.identity ----
    //
    // Single round-trip the MCP server uses to confirm a socket at
    // /tmp/vedit-*.sock actually belongs to a vedit instance (vs. a
    // stray file), and to tell a multi-instance UI which window the
    // agent is talking to. The socket path mirrors main.cpp; if the
    // path construction ever diverges, factor it into AppController.
    d.register_method("app.identity", [&](const nlohmann::json&) {
        int pid = ::getpid();
        std::string socket = std::string("/tmp/vedit-")
                           + std::to_string(pid) + ".sock";
        return nlohmann::json{
            {"name",   "vedit"},
            {"pid",    pid},
            {"socket", socket},
            {"label",  std::string("vedit:") + std::to_string(pid)}
        };
    });

    // ---- state.get ----
    d.register_method("state.get", [&](const nlohmann::json&) {
        return nlohmann::json::parse(ProjectSaveLoad::state_json(app.project()));
    });

    // ---- library.import ----
    //
    // Optional stream selection at import time. video_index / audio_index
    // default to -1 (MLT picks the first stream of each type). Pass >= 0
    // to select a specific stream from a multi-stream file (e.g. an mp4
    // with two audio tracks). audio_index == -2 selects "all" audio
    // streams (MLT decodes every audio stream interleaved).
    d.register_method("library.import", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        int video_index = params.value("video_index", -1);
        int audio_index = params.value("audio_index", -1);
        int idx = app.library_import(path, video_index, audio_index);
        if (idx < 0) throw RpcException(ErrorCode::ApplicationError, "import failed");
        return nlohmann::json{{"index", idx}};
    });

    // ---- library.remove ----
    d.register_method("library.remove", [&](const nlohmann::json& params) {
        int idx = params.value("index", -1);
        if (idx < 0) throw RpcException(ErrorCode::InvalidParams, "missing index");
        if (!app.library_remove(idx))
            throw RpcException(ErrorCode::ApplicationError,
                               "remove failed (index out of range or source in use)");
        return nlohmann::json{{"ok", true}};
    });

    // ---- library.set_stream ----
    //
    // Switch the selected video/audio stream on an imported source.
    // Refuses if the timeline still references it (per-media design:
    // all clips from one library item share the chosen streams, so
    // switching streams with clips in place would silently mutate them;
    // caller must drop the clips first).
    d.register_method("library.set_stream", [&](const nlohmann::json& params) {
        int idx = params.value("index", -1);
        if (idx < 0) throw RpcException(ErrorCode::InvalidParams, "missing index");
        int video_index = params.value("video_index", -1);
        int audio_index = params.value("audio_index", -1);
        if (!app.library_set_stream(idx, video_index, audio_index))
            throw RpcException(ErrorCode::ApplicationError,
                               "set_stream failed (bad index or source in use)");
        return nlohmann::json{{"ok", true}};
    });

    // ---- track.add / track.remove ----
    d.register_method("track.add", [&](const nlohmann::json& params) {
        std::string kind = params.value("kind", "video");
        std::string name = params.value("name", "");
        auto tk = kind == "audio" ? TrackKind::Audio : TrackKind::Video;
        auto id = app.track_add(tk, name);
        if (!id.valid()) throw RpcException(ErrorCode::ApplicationError, "track add failed");
        return nlohmann::json{{"track_id", id.value}};
    });

    d.register_method("track.remove", [&](const nlohmann::json& params) {
        TrackId id{params.value("track_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing track_id");
        if (!app.track_remove(id)) throw RpcException(ErrorCode::ApplicationError, "remove failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- clip.append / clip.remove / clip.move / clip.trim / clip.split ----
    d.register_method("clip.append", [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        int mi = params.value("media_index", -1);
        int64_t in = params.value("in", -1LL);
        int64_t out = params.value("out", -1LL);
        auto id = app.clip_append(tid, mi, in, out);
        if (!id.valid()) throw RpcException(ErrorCode::ApplicationError, "append failed");
        return nlohmann::json{{"clip_id", id.value}};
    });

    d.register_method("clip.insert", [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        int mi = params.value("media_index", -1);
        int64_t pos = params.value("position", 0LL);
        int64_t in_point = params.value("in", -1LL);
        int64_t out_point = params.value("out", -1LL);
        auto id = app.clip_insert(tid, mi, pos, in_point, out_point);
        if (!id.valid()) throw RpcException(ErrorCode::ApplicationError, "insert failed");
        return nlohmann::json{{"clip_id", id.value}};
    });

    d.register_method("clip.remove", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!app.clip_remove(id)) throw RpcException(ErrorCode::ApplicationError, "remove failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.move", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        TrackId tid{params.value("dest_track_id", 0LL)};
        int64_t pos = params.value("position", 0LL);
        if (!app.clip_move(id, tid, pos)) throw RpcException(ErrorCode::ApplicationError, "move failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.trim", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        int64_t in = params.value("in", 0LL);
        int64_t out = params.value("out", 0LL);
        if (!app.clip_trim(id, in, out)) throw RpcException(ErrorCode::ApplicationError, "trim failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.split", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        int64_t pos = params.value("at_position", 0LL);
        auto new_id = app.clip_split(id, pos);
        if (!new_id.valid()) throw RpcException(ErrorCode::ApplicationError, "split failed");
        return nlohmann::json{{"clip_id", new_id.value}};
    });

    // ---- player.play / player.pause / player.seek ----
    d.register_method("player.play", [&](const nlohmann::json&) {
        if (!app.play()) throw RpcException(ErrorCode::ApplicationError, "play failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("player.pause", [&](const nlohmann::json&) {
        app.pause();
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("player.seek", [&](const nlohmann::json& params) {
        int64_t pos = params.value("position", 0LL);
        app.seek(pos);
        return nlohmann::json{{"position", app.position()}};
    });

    // ---- player.get_frame ----
    //
    // Pull a single composed frame from the timeline tractor at
    // `position` and write it to `path` as a PNG. This is the brief's
    // "single-frame interface" — the primitive an AI model calls to
    // inspect a frame. Uses ExportService::get_frame over the project's
    // tractor, which is the same assembly the preview path renders.
    d.register_method("player.get_frame", [&](const nlohmann::json& params) {
        int64_t pos = params.value("position", app.position());
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        ExportService svc;
        if (!svc.get_frame(*app.project().producer(), app.profile(), pos, path))
            throw RpcException(ErrorCode::ApplicationError, "get_frame failed");
        return nlohmann::json{{"path", path}, {"position", pos}};
    });

    // ---- view.screenshot ----
    //
    // Same single-frame PNG, but pinned to the current playhead position
    // so an agent can "see what the user sees" without needing to know
    // the frame number.
    d.register_method("view.screenshot", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        int64_t pos = app.position();
        ExportService svc;
        if (!svc.get_frame(*app.project().producer(), app.profile(), pos, path))
            throw RpcException(ErrorCode::ApplicationError, "screenshot failed");
        return nlohmann::json{{"path", path}, {"position", pos}};
    });

    // ---- project.save / project.open ----
    d.register_method("project.save", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        if (!ProjectSaveLoad::save_xml(app.project(), path))
            throw RpcException(ErrorCode::ApplicationError, "save failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("project.open", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        if (!ProjectSaveLoad::load_xml(app.project(), path))
            throw RpcException(ErrorCode::ApplicationError, "open failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- export.start / export.poll / export.cancel ----
    //
    // Async export. export.start kicks off a worker-thread render and
    // returns a job id immediately; export.poll reports state/fraction;
    // export.cancel signals cancellation (best-effort — MLT's run()
    // blocks until terminate_on_pause, so a long encode can't be
    // interrupted mid-stream, but the request is recorded and the
    // final state becomes "cancelled").
    d.register_method("export.start", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        int64_t job_id = app.export_start(path);
        if (job_id == 0) throw RpcException(ErrorCode::ApplicationError, "export start failed");
        return nlohmann::json{{"job_id", job_id}};
    });

    d.register_method("export.poll", [&](const nlohmann::json& params) {
        int64_t job_id = params.value("job_id", 0LL);
        if (job_id <= 0) throw RpcException(ErrorCode::InvalidParams, "missing job_id");
        auto r = app.export_poll(job_id);
        if (!r.found) throw RpcException(ErrorCode::ApplicationError, "unknown job_id");
        return nlohmann::json{
            {"state",  r.state},
            {"fraction", r.fraction},
            {"path",   r.output_path},
            {"error",  r.error}
        };
    });

    d.register_method("export.cancel", [&](const nlohmann::json& params) {
        int64_t job_id = params.value("job_id", 0LL);
        if (job_id <= 0) throw RpcException(ErrorCode::InvalidParams, "missing job_id");
        if (!app.export_cancel(job_id))
            throw RpcException(ErrorCode::ApplicationError, "cancel failed (unknown id or not running)");
        return nlohmann::json{{"ok", true}};
    });
}

} // namespace vedit::rpc
