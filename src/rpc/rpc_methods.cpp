#include "rpc_methods.h"

#include "rpc_dispatcher.h"
#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/project_save_load.h"
#include "rpc_methods.h"

#include "rpc_dispatcher.h"
#include "domain/app_controller.h"
#include "domain/export_service.h"
#include "domain/ids.h"
#include "domain/project_save_load.h"
#include "util/platform_compat.h"

#ifndef _WIN32
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <glob.h>
#endif
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace vedit::rpc {

void register_methods(Dispatcher& d, AppController& app) {
    // ---- app.identity ----
    //
    // Single round-trip the MCP server uses to confirm a socket at
    // vedit-*.sock actually belongs to a vedit instance (vs. a stray
    // file), and to tell a multi-instance UI which window the agent is
    // talking to. The socket path comes from vedit::default_socket_path()
    // so it stays in sync with main.cpp.
    d.register_method("app.identity", [&](const nlohmann::json&) {
        int pid = vedit::current_pid();
        std::string socket = vedit::default_socket_path();
        return nlohmann::json{
            {"name",   "vedit"},
            {"pid",    pid},
            {"socket", socket},
            {"label",  std::string("vedit:") + std::to_string(pid)}
        };
    });

    // ---- app.list_instances ----
    //
    // Enumerate all running vedit instances on this host. Globs the
    // temp directory for vedit-*.sock, probes each with app.identity
    // over a brief JSON-RPC handshake, and returns the alive list.
    // Used by the MCP shim (vedit_client.discover_instances) and by
    // multi-instance UIs.
    d.register_method("app.list_instances", [&](const nlohmann::json&) {
        nlohmann::json instances = nlohmann::json::array();
#ifndef _WIN32
        // Use the same temp directory that default_socket_path() uses,
        // not hardcoded /tmp (macOS uses a per-user temp dir).
        std::string glob_pattern =
            (vedit::temp_dir() / "vedit-*.sock").string();
        glob_t g;
        std::memset(&g, 0, sizeof(g));
        if (::glob(glob_pattern.c_str(), 0, nullptr, &g) != 0) {
            return instances;
        }

        std::string self_path = vedit::default_socket_path();

        for (std::size_t i = 0; i < g.gl_pathc; ++i) {
            std::string path = g.gl_pathv[i];
            if (path == self_path) continue;

            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;

            // Bound the handshake. A socket file left behind by a crashed
            // instance still accepts connects but never replies, and a
            // blocking recv without a timeout would hang the caller.
            struct timeval tv = {0, 500000};  // 500 ms
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct sockaddr_un addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr)) != 0) {
                ::close(fd);
                continue;
            }

            // Send a framed app.identity request and read back the
            // response. This is a one-shot synchronous handshake; the
            // request body is tiny (< 256 bytes) and the response
            // equally small, so blocking I/O is fine here.
            std::string req_body = R"({"jsonrpc":"2.0","method":"app.identity","id":1})";
            std::uint32_t req_len_be = htonl(
                static_cast<std::uint32_t>(req_body.size()));
            if (::send(fd, &req_len_be, 4, MSG_NOSIGNAL) != 4 ||
                ::send(fd, req_body.data(), req_body.size(), MSG_NOSIGNAL)
                    != static_cast<ssize_t>(req_body.size())) {
                ::close(fd);
                continue;
            }

            std::uint32_t resp_len_be = 0;
            if (::recv(fd, &resp_len_be, 4, 0) != 4) {
                ::close(fd);
                continue;
            }
            std::uint32_t resp_len = ntohl(resp_len_be);
            if (resp_len == 0 || resp_len > 65536) {
                ::close(fd);
                continue;
            }
            std::string resp_body(resp_len, '\0');
            if (::recv(fd, &resp_body[0], resp_len, 0)
                    != static_cast<ssize_t>(resp_len)) {
                ::close(fd);
                continue;
            }
            ::close(fd);

            try {
                nlohmann::json parsed = nlohmann::json::parse(resp_body);
                if (parsed.contains("result") && parsed["result"].contains("name")) {
                    instances.push_back(parsed["result"]);
                }
            } catch (...) {
                // Malformed response from a stale/incompatible socket —
                // skip this entry.
            }
        }
        ::globfree(&g);
#endif  // !_WIN32
        return instances;
    });

    // ---- filter.list ----
    //
    // Returns every MLT filter service known to the repository, with
    // yml-driven parameter metadata. The agent uses this to discover
    // what filters are available and what parameters they accept before
    // calling filter.add / filter.set_param.
    d.register_method("filter.list", [&](const nlohmann::json&) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& f : app.filter_catalog().filters()) {
            nlohmann::json j;
            j["id"] = f.id;
            j["title"] = f.title;
            j["description"] = f.description;
            j["is_audio"] = f.is_audio;
            j["is_video"] = f.is_video;
            nlohmann::json params = nlohmann::json::array();
            for (const auto& p : f.params) {
                nlohmann::json pj;
                pj["identifier"] = p.identifier;
                pj["title"] = p.title;
                pj["type"] = p.type;
                pj["default"] = p.default_value;
                pj["minimum"] = p.minimum;
                pj["maximum"] = p.maximum;
                pj["mutable"] = p.mutable_;
                pj["animation"] = p.animation;
                if (!p.widget.empty()) pj["widget"] = p.widget;
                if (!p.values.empty()) pj["values"] = p.values;
                params.push_back(std::move(pj));
            }
            j["params"] = std::move(params);
            result.push_back(std::move(j));
        }
        return result;
    });

    // ---- filter.add / filter.remove / filter.set_param / filter.get_param ----
    //
    // Target: clip_id (per-clip) or track_id (per-track). Per-clip
    // attaches to the cut producer; per-track plants on the tractor
    // field and applies to all clips on that track.
    // Optional in/out (frame numbers, clip-relative) set the filter's
    // active range via MLT's set_in_and_out.
    d.register_method("filter.add", [&](const nlohmann::json& params) {
        std::string name = params.value("filter", "");
        if (name.empty()) throw RpcException(ErrorCode::InvalidParams, "missing filter name");
        int in = params.value("in", -1);
        int out = params.value("out", -1);

        if (params.contains("track_id")) {
            TrackId tid{params.value("track_id", 0LL)};
            if (!app.track_filter_add(tid, name))
                throw RpcException(ErrorCode::ApplicationError, "add to track failed");
        } else {
            ClipId id{params.value("clip_id", 0LL)};
            if (!app.filter_add(id, name, in, out))
                throw RpcException(ErrorCode::ApplicationError, "add to clip failed");
        }
        return nlohmann::json{{"ok", true}};
    });

    // ---- filter.set_range ----
    //
    // Set the active frame range on an already-attached filter.
    // in/out are clip-relative frame numbers.
    d.register_method("filter.set_range", [&](const nlohmann::json& params) {
        std::string filter_name = params.value("filter", "");
        if (filter_name.empty())
            throw RpcException(ErrorCode::InvalidParams, "missing filter name");
        if (!params.contains("in") || !params.contains("out"))
            throw RpcException(ErrorCode::InvalidParams, "missing in/out");
        int in = params.value("in", 0);
        int out = params.value("out", 0);

        ClipId id{params.value("clip_id", 0LL)};
        if (!app.filter_set_in_out(id, filter_name, in, out))
            throw RpcException(ErrorCode::ApplicationError, "set_range failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("filter.remove", [&](const nlohmann::json& params) {
        std::string name = params.value("filter", "");
        if (name.empty()) throw RpcException(ErrorCode::InvalidParams, "missing filter name");

        if (params.contains("track_id")) {
            TrackId tid{params.value("track_id", 0LL)};
            if (!app.track_filter_remove(tid, name))
                throw RpcException(ErrorCode::ApplicationError, "remove from track failed");
        } else {
            ClipId id{params.value("clip_id", 0LL)};
            if (!app.filter_remove(id, name))
                throw RpcException(ErrorCode::ApplicationError, "remove from clip failed");
        }
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("filter.set_param", [&](const nlohmann::json& params) {
        std::string filter_name = params.value("filter", "");
        std::string param = params.value("param", "");
        if (filter_name.empty() || param.empty())
            throw RpcException(ErrorCode::InvalidParams, "missing filter or param");

        bool is_track = params.contains("track_id");
        bool ok = false;

        // P2 shape: {keyframes: [{frame, value, type}, ...]}
        if (params.contains("keyframes")) {
            const auto& kfs = params["keyframes"];
            if (is_track) {
                TrackId tid{params.value("track_id", 0LL)};
                ok = app.track_filter_set_param_keyframes(
                    tid, filter_name, param, kfs);
            } else {
                ClipId id{params.value("clip_id", 0LL)};
                ok = app.filter_set_param_keyframes(
                    id, filter_name, param, kfs);
            }
        } else if (params.contains("value")) {
            // P2 shape: {value: <scalar>}
            std::string value;
            const auto& v = params["value"];
            value = v.is_string() ? v.get<std::string>() : v.dump();
            if (is_track) {
                TrackId tid{params.value("track_id", 0LL)};
                ok = app.track_filter_set_param(
                    tid, filter_name, param, value);
            } else {
                ClipId id{params.value("clip_id", 0LL)};
                ok = app.filter_set_param(
                    id, filter_name, param, value);
            }
        } else {
            throw RpcException(ErrorCode::InvalidParams,
                               "missing value or keyframes");
        }

        if (!ok)
            throw RpcException(ErrorCode::ApplicationError,
                               "set_param failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("filter.get_param", [&](const nlohmann::json& params) {
        std::string filter_name = params.value("filter", "");
        std::string param = params.value("param", "");
        if (filter_name.empty() || param.empty())
            throw RpcException(ErrorCode::InvalidParams, "missing filter or param");

        nlohmann::json result;
        if (params.contains("track_id")) {
            TrackId tid{params.value("track_id", 0LL)};
            result = app.track_filter_get_param_keyframes(
                tid, filter_name, param);
        } else {
            ClipId id{params.value("clip_id", 0LL)};
            result = app.filter_get_param_keyframes(
                id, filter_name, param);
        }

        if (result.empty())
            throw RpcException(ErrorCode::ApplicationError,
                               "param not found or filter not attached");
        return result;
    });

    // ---- state.get ----
    d.register_method("state.get", [&](const nlohmann::json&) {
        return nlohmann::json::parse(
            ProjectSaveLoad::state_json(app.project(),
                                        app.filter_catalog()));
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

    d.register_method("track.set_muted", [&](const nlohmann::json& params) {
        TrackId id{params.value("track_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing track_id");
        bool muted = params.value("muted", true);
        if (!app.track_set_muted(id, muted))
            throw RpcException(ErrorCode::ApplicationError, "set_muted failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("track.set_hidden", [&](const nlohmann::json& params) {
        TrackId id{params.value("track_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing track_id");
        bool hidden = params.value("hidden", true);
        if (!app.track_set_hidden(id, hidden))
            throw RpcException(ErrorCode::ApplicationError, "set_hidden failed");
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

    d.register_method("clip.overwrite", [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        int mi = params.value("media_index", -1);
        int64_t pos = params.value("position", 0LL);
        int64_t in = params.value("in", -1LL);
        int64_t out = params.value("out", -1LL);
        auto id = app.clip_overwrite(tid, mi, pos, in, out);
        if (!id.valid())
            throw RpcException(ErrorCode::ApplicationError, "overwrite failed");
        return nlohmann::json{{"clip_id", id.value}};
    });

    d.register_method("clip.lift", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!app.clip_lift(id))
            throw RpcException(ErrorCode::ApplicationError, "lift failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.change_speed", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        double speed = params.value("speed", 1.0);
        if (!app.clip_change_speed(id, speed))
            throw RpcException(ErrorCode::ApplicationError, "change_speed failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.detach_audio", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        auto new_id = app.clip_detach_audio(id);
        if (!new_id.valid())
            throw RpcException(ErrorCode::ApplicationError, "detach_audio failed");
        return nlohmann::json{{"clip_id", new_id.value}};
    });

    // ---- clip.set_audio_enabled / clip.set_video_enabled ----
    //
    // Mute or unmute the audio or video stream on a clip. The clip
    // stays in the timeline; only the stream is toggled. The effect
    // is visible in preview and export.
    d.register_method("clip.set_audio_enabled", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        bool enabled = params.value("enabled", true);
        if (!app.clip_set_audio_enabled(id, enabled))
            throw RpcException(ErrorCode::ApplicationError,
                               "set_audio_enabled failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.set_video_enabled", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        bool enabled = params.value("enabled", true);
        if (!app.clip_set_video_enabled(id, enabled))
            throw RpcException(ErrorCode::ApplicationError,
                               "set_video_enabled failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- clip.set_geometry / clip.get_geometry / clip.set_rotation ----
    //
    // Per-clip position, size, and rotation via the MLT affine filter.
    // x, y, w, h are percentages (0-100) of the profile resolution.
    // All params are optional on set (partial update); defaults fill in
    // from the current geometry. Rotation is in degrees, clockwise.
    d.register_method("clip.set_geometry", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        double cx, cy, cw, ch;
        if (!app.clip_get_geometry(id, cx, cy, cw, ch))
            throw RpcException(ErrorCode::InvalidParams, "unknown clip_id");
        double x = params.contains("x") ? params["x"].get<double>() : cx;
        double y = params.contains("y") ? params["y"].get<double>() : cy;
        double w = params.contains("w") ? params["w"].get<double>() : cw;
        double h = params.contains("h") ? params["h"].get<double>() : ch;
        if (!app.clip_set_geometry(id, x, y, w, h))
            throw RpcException(ErrorCode::ApplicationError, "set_geometry failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.get_geometry", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        double x, y, w, h;
        if (!app.clip_get_geometry(id, x, y, w, h))
            throw RpcException(ErrorCode::InvalidParams, "unknown clip_id");
        return nlohmann::json{{"x", x}, {"y", y}, {"w", w}, {"h", h}};
    });

    d.register_method("clip.get_rotation", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        double degrees = 0.0;
        if (!app.clip_get_rotation(id, degrees))
            throw RpcException(ErrorCode::InvalidParams, "unknown clip_id");
        return nlohmann::json{{"degrees", degrees}};
    });

    d.register_method("clip.set_rotation", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        double degrees = params.value("degrees", 0.0);
        if (!app.clip_set_rotation(id, degrees))
            throw RpcException(ErrorCode::ApplicationError, "set_rotation failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- clip.set_crop / clip.get_crop ----
    //
    // Per-clip crop margins in pixels. All params are optional on set
    // (partial update); defaults fill in from the current crop values.
    d.register_method("clip.set_crop", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        int cl, cr, ct, cb;
        if (!app.clip_get_crop(id, cl, cr, ct, cb))
            throw RpcException(ErrorCode::InvalidParams, "unknown clip_id");
        int left   = params.value("left",   cl);
        int right  = params.value("right",  cr);
        int top    = params.value("top",    ct);
        int bottom = params.value("bottom", cb);
        if (!app.clip_set_crop(id, left, right, top, bottom))
            throw RpcException(ErrorCode::ApplicationError, "set_crop failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.get_crop", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        int left, right, top, bottom;
        if (!app.clip_get_crop(id, left, right, top, bottom))
            throw RpcException(ErrorCode::InvalidParams, "unknown clip_id");
        return nlohmann::json{{"left", left}, {"right", right},
                              {"top", top},   {"bottom", bottom}};
    });

    // ---- clip.set_fade_in / clip.set_fade_out ----
    //
    // Per-clip fade-in/fade-out via brightness + volume keyframes.
    // duration_frames=0 removes the fade. Fade-in animates from 0 to 1
    // at the clip start; fade-out animates from 1 to 0 at the clip end.
    d.register_method("clip.set_fade_in", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        int duration = params.value("duration", 0);
        if (!app.clip_set_fade_in(id, duration))
            throw RpcException(ErrorCode::ApplicationError, "set_fade_in failed");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("clip.set_fade_out", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        int duration = params.value("duration", 0);
        if (!app.clip_set_fade_out(id, duration))
            throw RpcException(ErrorCode::ApplicationError, "set_fade_out failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- analysis.track ----
    //
    // Run opencv.tracker on a clip and return per-frame rect keyframes.
    // The initial rect is "x y w h" (pixels at clip resolution).
    // algo selects the OpenCV tracker (KCF, MIL, CSRT, MOSSE, ...).
    // steps controls keyframe spacing (every N frames, default 5).
    d.register_method("analysis.track", [&](const nlohmann::json& params) {
        ClipId id{params.value("clip_id", 0LL)};
        if (!id.valid()) throw RpcException(ErrorCode::InvalidParams, "missing clip_id");
        std::string rect = params.value("rect", "0 0 50 50");
        std::string algo = params.value("algo", "KCF");
        int steps = params.value("steps", 5);
        auto result = app.clip_analyze_track(id, rect, algo, steps);
        return nlohmann::json{{"keyframes", std::move(result)}};
    });

    d.register_method("transition.add", [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        int clip_idx = params.value("clip_index", 0);
        int length = params.value("length", 30);
        std::string type = params.value("type", "luma");
        if (!app.clip_add_transition(tid, clip_idx, length, type))
            throw RpcException(ErrorCode::ApplicationError, "transition.add failed");
        return nlohmann::json{{"ok", true}};
    });

    // ---- transition.list_types ----
    // Reports the transition types add_transition actually wires up, not
    // every MLT transition service. Keeping this honest means an agent's
    // type string is guaranteed to produce a visible result.
    d.register_method("transition.list_types", [&](const nlohmann::json&) {
        auto entry = [](const char* id, const char* title,
                        const char* desc) {
            return nlohmann::json{{"id", id}, {"title", title},
                                  {"description", desc}};
        };
        return nlohmann::json::array({
            entry("dissolve", "Cross dissolve",
                  "Soft luma dissolve plus audio cross-fade"),
            entry("luma", "Luma wipe",
                  "Diagonal luma wipe plus audio cross-fade"),
            entry("bar", "Bar wipe", "Bar luma pattern wipe"),
            entry("barn", "Barn door", "Barn-door luma pattern wipe"),
            entry("clock", "Clock wipe", "Clock luma pattern wipe"),
            entry("iris", "Iris", "Iris luma pattern wipe"),
            entry("spiral", "Spiral", "Spiral luma pattern wipe"),
        });
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

    // ---- undo / redo ----
    //
    // Same history the GUI drives, because both go through AppController.
    // A restore re-mints clip and track ids, so the caller must re-read
    // state.get before addressing clips again.
    d.register_method("undo", [&](const nlohmann::json&) {
        if (!app.undo())
            throw RpcException(ErrorCode::ApplicationError, "nothing to undo");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("redo", [&](const nlohmann::json&) {
        if (!app.redo())
            throw RpcException(ErrorCode::ApplicationError, "nothing to redo");
        return nlohmann::json{{"ok", true}};
    });

    // ---- project.new / project.save / project.open ----
    //
    // project.new clears the timeline and library so an agent can start
    // a fresh editing session without quitting and re-launching vedit.
    d.register_method("project.new", [&](const nlohmann::json&) {
        app.project_new();
        return nlohmann::json{{"ok", true}};
    });

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

    // ---- project.set_profile ----
    //
    // Set the output canvas resolution and framerate. Call before
    // placing clips; the profile determines render dimensions. Width
    // and height are pixels; fps is frames per second (accepts 29.97
    // etc., rounded to rational internally).
    d.register_method("project.set_profile", [&](const nlohmann::json& params) {
        int width = params.value("width", 0);
        int height = params.value("height", 0);
        double fps = params.value("fps", 0.0);
        if (width <= 0 || height <= 0 || fps <= 0)
            throw RpcException(ErrorCode::InvalidParams,
                               "width, height, and fps are required");
        app.profile_set(width, height, fps);
        return nlohmann::json{{"ok", true}};
    });

    // ---- track.set_composite_geometry ----
    //
    // Set the geometry rect for a video track's composite transition.
    // x, y, w, h are percentages (0-100) of the profile resolution.
    // This positions the track's content on the canvas. Track 0 has
    // no composite (it fills the canvas as background).
    d.register_method("track.set_composite_geometry",
                      [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        double x = params.value("x", 0.0);
        double y = params.value("y", 0.0);
        double w = params.value("w", 100.0);
        double h = params.value("h", 100.0);
        if (!app.composite_geometry_set(tid, x, y, w, h))
            throw RpcException(ErrorCode::ApplicationError,
                               "set composite geometry failed "
                               "(not a video track, or track 0)");
        return nlohmann::json{{"ok", true}};
    });

    d.register_method("track.get_composite_geometry",
                      [&](const nlohmann::json& params) {
        TrackId tid{params.value("track_id", 0LL)};
        double x = 0, y = 0, w = 100, h = 100;
        if (!app.composite_geometry_get(tid, x, y, w, h))
            throw RpcException(ErrorCode::ApplicationError,
                               "get composite geometry failed "
                               "(not a video track, or track 0)");
        return nlohmann::json{{"x", x}, {"y", y}, {"w", w}, {"h", h}};
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
        nlohmann::json codec_params = params.value("params", nlohmann::json::object());
        int64_t in = params.value("in", -1LL);
        int64_t out = params.value("out", -1LL);
        int64_t job_id = app.export_start(path, codec_params, in, out);
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

    d.register_method("export.list_presets", [&](const nlohmann::json&) {
        auto presets = ExportService::list_presets();
        nlohmann::json result = nlohmann::json::array();
        for (const auto& p : presets) {
            nlohmann::json pj;
            pj["name"] = p.name;
            pj["extension"] = p.extension;
            pj["description"] = p.description;
            pj["file"] = p.file_path;
            result.push_back(std::move(pj));
        }
        return result;
    });

    d.register_method("export.load_preset", [&](const nlohmann::json& params) {
        std::string path = params.value("file", "");
        if (path.empty())
            throw RpcException(ErrorCode::InvalidParams, "missing file");
        return ExportService::load_preset(path);
    });
}

} // namespace vedit::rpc
