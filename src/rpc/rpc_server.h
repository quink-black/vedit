#ifndef VEDIT_RPC_SERVER_H
#define VEDIT_RPC_SERVER_H

#include <cstddef>
#include <memory>
#include <string>

namespace vedit::rpc {

class Dispatcher;

// Maximum bytes accepted in a single framed request.  64 MiB is well
// above realistic state.get / screenshot payloads, but keeps a hostile
// or buggy client from forcing a multi-GiB allocation via a crafted
// length prefix.
inline constexpr std::size_t kMaxFrameBytes = 64ull * 1024 * 1024;

// JSON-RPC 2.0 transport over Unix Domain Socket (POSIX).
//
// Architecture (single-state multi-channel, ported from idiff):
//
//   GUI thread (main)                      Asio I/O thread
//   ----------------                       ---------------
//   App::frame() loop                      io_context::run()
//      |                                       |
//      | RpcServer::drain()                    | accept / read frames
//      v                                       v
//   pop pending request -----promise<-----  push (req_json, promise)
//   Dispatcher::handle_request                 |  await future
//   set promise value -----future----------->  | write framed response
//
// Invariants:
//   * The I/O thread NEVER touches AppController / MLT / OpenGL. It
//     only owns sockets, framing buffers, and the request queue.
//   * drain() is the ONLY place Dispatcher handlers run, called from
//     the main thread. Handlers may freely access App state without
//     further synchronisation.
//   * Wire format: 4-byte big-endian length prefix, then JSON. 64 MiB
//     max per frame; oversize frames close the connection.
//   * Notifications: handle_request returns "" -> we write nothing,
//     keep the connection open.
//
// Sequencing contract:
//   * Requests are dispatched strictly one at a time. The I/O thread
//     blocks on each request's future until drain() fulfills it, so a
//     second in-flight request on any session is NOT read until the
//     first completes. This is intentional: UI operations and AI-agent
//     (MCP) access must run in sequence to keep app state
//     deterministic. Clients that pipeleline two requests concurrently
//     will deadlock. Send one request, wait for its response, then
//     send the next.
//
// PIMPL: all Asio types are confined to rpc_server.cpp. Consumers
// (main, tests) only see std::string socket paths and a Dispatcher
// reference.
class RpcServer {
public:
    // Build a server bound to `socket_path` that will dispatch through
    // `dispatcher`. The Dispatcher must outlive this RpcServer.
    // Construction does not open the socket; call start().
    RpcServer(std::string socket_path, Dispatcher& dispatcher);

    // Stops the server if running.
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // Bind, listen, and spawn the I/O thread. Unlinks any stale socket
    // file at `socket_path` first (typical for a path left over from a
    // previous crashed instance).
    //
    // Additionally sweeps sibling sockets matching the `vedit-*.sock`
    // pattern in the temp dir (vedit::temp_dir()), probing each via
    // connect(2); any that return ECONNREFUSED are unlinked as stale.
    //
    // Throws std::system_error on bind/listen failure. Idempotent if
    // already running (no-op).
    void start();

    // Stop accepting new connections, close existing sessions, and
    // join the I/O thread. Safe to call from the main thread; the
    // destructor calls this automatically.
    void stop();

    // Process all currently queued requests on the calling thread.
    // Intended to be called once per GUI frame. Each pending request
    // is dispatched synchronously; the response is delivered back to
    // the originating Asio session via its waiting promise.
    //
    // Returns the number of requests dispatched in this call. Does
    // not block: if the queue is empty it returns 0.
    std::size_t drain();

    // Path the server is (or will be) bound to.
    const std::string& socket_path() const noexcept;

    // True once start() has succeeded and the I/O thread is running.
    bool is_running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vedit::rpc

#endif // VEDIT_RPC_SERVER_H
