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
    d.register_method("library.import", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        int idx = app.library_import(path);
        if (idx < 0) throw RpcException(ErrorCode::ApplicationError, "import failed");
        return nlohmann::json{{"index", idx}};
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
        auto id = app.clip_insert(tid, mi, pos);
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

    // ---- export.start ----
    d.register_method("export.start", [&](const nlohmann::json& params) {
        std::string path = params.value("path", "");
        if (path.empty()) throw RpcException(ErrorCode::InvalidParams, "missing path");
        ExportService svc;
        if (!svc.render(*app.project().producer(), app.profile(), path))
            throw RpcException(ErrorCode::ApplicationError, "export failed");
        return nlohmann::json{{"ok", true}};
    });
}

} // namespace vedit::rpc
