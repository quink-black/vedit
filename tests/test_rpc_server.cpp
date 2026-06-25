// Integration tests for RpcServer over a real Unix Domain Socket.
//
// These tests exercise the full transport path: a real RpcServer bound
// to a per-test temp socket path, a real UDS client using POSIX
// socket/recv, framed requests and framed responses flowing both ways.
// The dispatcher is the production vedit::rpc::Dispatcher.
//
// Coverage:
//   * Connect + echo (single request -> single response)
//   * Multiple concurrent clients each holding a session open
//   * Response framing (BE length prefix + JSON body)
//   * Stale-socket sweep on start()
//   * Notification (no id) -> no response, connection kept open
//
// Pattern: each test spawns the server, the test thread acts as the
// main thread calling drain() in a loop (typically on a short polling
// timer until the expected response arrives), and a helper UDS client
// sends framed requests and reads framed responses.

#include <catch2/catch_test_macros.hpp>

#include "rpc/rpc_dispatcher.h"
#include "rpc/rpc_server.h"
#include "util/platform_compat.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;

namespace {

// Per-test unique socket path under /tmp. Using the test PID + a
// counter avoids collisions when tests run in parallel within one
// process (Catch2 can do this) and survives across re-runs because
// start() also unlinks the path before binding.
std::string make_socket_path() {
    static std::atomic<int> counter{0};
    return "/tmp/vedit-test-" + std::to_string(::getpid()) + "-"
         + std::to_string(counter.fetch_add(1)) + ".sock";
}

// Build a length-prefixed frame: 4-byte BE length + JSON body.
std::string frame(const std::string& body) {
    std::string out;
    out.resize(4 + body.size());
    std::uint32_t len = static_cast<std::uint32_t>(body.size());
    out[0] = static_cast<char>((len >> 24) & 0xFF);
    out[1] = static_cast<char>((len >> 16) & 0xFF);
    out[2] = static_cast<char>((len >>  8) & 0xFF);
    out[3] = static_cast<char>( len        & 0xFF);
    out.replace(4, body.size(), body);
    return out;
}

// Read exactly n bytes from a blocking socket. Returns false on EOF/error.
bool recv_all(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// Read one framed response (4-byte BE length + body). Returns the
// body string, or empty string on EOF/error.
std::string recv_frame(int fd) {
    unsigned char len_buf[4];
    if (!recv_all(fd, len_buf, 4)) return {};
    std::uint32_t len = (static_cast<std::uint32_t>(len_buf[0]) << 24)
                      | (static_cast<std::uint32_t>(len_buf[1]) << 16)
                      | (static_cast<std::uint32_t>(len_buf[2]) <<  8)
                      |  static_cast<std::uint32_t>(len_buf[3]);
    if (len == 0 || len > 64u * 1024 * 1024) return {};
    std::string body(len, '\0');
    if (!recv_all(fd, body.data(), len)) return {};
    return body;
}

// Connect a UDS client to `path`. Returns fd or -1.
int connect_client(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Run a drain loop on the calling thread for up to `timeout_ms`,
// stopping early once `predicate` returns true. Calls drain() every
// 1 ms so handler latency stays low.
void drain_until(vedit::rpc::RpcServer& server,
                  std::function<bool()> predicate,
                  int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        server.drain();
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Final drain so the predicate check in the caller sees the latest.
    server.drain();
}

} // namespace

TEST_CASE("RpcServer echoes a request over a real UDS", "[rpc_server]") {
    vedit::rpc::Dispatcher d;
    d.register_method("test.echo", [](const json& params) {
        return json{{"received", params}};
    });
    std::string path = make_socket_path();
    vedit::rpc::RpcServer server(path, d);
    REQUIRE_NOTHROW(server.start());
    REQUIRE(server.is_running());

    int fd = connect_client(path);
    REQUIRE(fd >= 0);

    std::string req = R"({"jsonrpc":"2.0","method":"test.echo","params":{"x":7},"id":1})";
    std::string framed = frame(req);
    REQUIRE(::send(fd, framed.data(), framed.size(), 0)
            == static_cast<ssize_t>(framed.size()));

    std::string response_body;
    drain_until(server, [&] {
        // Drain happens inside drain_until; we just need to attempt
        // the read once the server has had a chance to dispatch.
        return false;
    }, 200);
    response_body = recv_frame(fd);
    REQUIRE_FALSE(response_body.empty());

    auto parsed = json::parse(response_body);
    REQUIRE(parsed["jsonrpc"] == "2.0");
    REQUIRE(parsed["id"] == 1);
    REQUIRE(parsed["result"]["received"]["x"] == 7);

    ::close(fd);
    server.stop();
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("RpcServer app.identity round-trip", "[rpc_server]") {
    // The MCP shim probes each /tmp/vedit-*.sock with app.identity to
    // confirm it belongs to a vedit instance. Verify the handshake
    // shape: name, pid, socket, label.
    vedit::rpc::Dispatcher d;
    d.register_method("app.identity", [](const json&) {
        return json{{"name", "vedit"}, {"pid", 12345},
                    {"socket", "/tmp/vedit-12345.sock"},
                    {"label", "vedit:12345"}};
    });
    std::string path = make_socket_path();
    vedit::rpc::RpcServer server(path, d);
    server.start();

    int fd = connect_client(path);
    REQUIRE(fd >= 0);

    std::string req = R"({"jsonrpc":"2.0","method":"app.identity","id":1})";
    auto framed = frame(req);
    REQUIRE(::send(fd, framed.data(), framed.size(), 0)
            == static_cast<ssize_t>(framed.size()));

    drain_until(server, [&] { return false; }, 200);
    std::string body = recv_frame(fd);
    REQUIRE_FALSE(body.empty());

    auto parsed = json::parse(body);
    REQUIRE(parsed["jsonrpc"] == "2.0");
    REQUIRE(parsed["id"] == 1);
    REQUIRE(parsed["result"]["name"] == "vedit");
    REQUIRE(parsed["result"]["socket"] == "/tmp/vedit-12345.sock");
    REQUIRE(parsed["result"]["label"] == "vedit:12345");

    ::close(fd);
    server.stop();
}

TEST_CASE("RpcServer services sequential requests from multiple clients",
          "[rpc_server]") {
    // The server dispatches exactly one request at a time: the I/O
    // thread blocks on each request's future until drain() fulfills
    // it, so a second in-flight request is not read until the first
    // completes. This is intentional -- UI and AI-agent access must
    // stay sequential to keep app state deterministic. Pipelining two
    // requests concurrently would deadlock. Verify the supported
    // pattern: two clients connect, each sends one request and waits
    // for its response before the next client sends.
    vedit::rpc::Dispatcher d;
    d.register_method("test.echo", [](const json& params) {
        return json{{"ok", true}, {"n", params.value("n", 0)}};
    });
    std::string path = make_socket_path();
    vedit::rpc::RpcServer server(path, d);
    server.start();

    int fd_a = connect_client(path);
    int fd_b = connect_client(path);
    REQUIRE(fd_a >= 0);
    REQUIRE(fd_b >= 0);

    // Client A sends; drain for a bit so the server dispatches and
    // writes the response; then read it. recv_frame is called only
    // after draining (not inside the drain_until predicate) so it
    // can never block the main thread before drain() has run.
    std::string req_a = R"({"jsonrpc":"2.0","method":"test.echo","params":{"n":1},"id":1})";
    auto fa = frame(req_a);
    REQUIRE(::send(fd_a, fa.data(), fa.size(), 0) == fa.size());
    drain_until(server, [&] { return false; }, 200);
    std::string ra = recv_frame(fd_a);
    REQUIRE_FALSE(ra.empty());
    REQUIRE(json::parse(ra)["result"]["n"] == 1);

    // Client B sends after A's response is complete.
    std::string req_b = R"({"jsonrpc":"2.0","method":"test.echo","params":{"n":2},"id":2})";
    auto fb = frame(req_b);
    REQUIRE(::send(fd_b, fb.data(), fb.size(), 0) == fb.size());
    drain_until(server, [&] { return false; }, 200);
    std::string rb = recv_frame(fd_b);
    REQUIRE_FALSE(rb.empty());
    REQUIRE(json::parse(rb)["result"]["n"] == 2);

    ::close(fd_a);
    ::close(fd_b);
    server.stop();
}

TEST_CASE("RpcServer handles notification (no id) without responding",
          "[rpc_server]") {
    vedit::rpc::Dispatcher d;
    auto called = std::make_shared<bool>(false);
    d.register_method("notify.test", [called](const json&) {
        *called = true;
        return json{{"ok", true}};
    });
    std::string path = make_socket_path();
    vedit::rpc::RpcServer server(path, d);
    server.start();

    int fd = connect_client(path);
    REQUIRE(fd >= 0);

    // Notification: no "id" field.
    std::string req = R"({"jsonrpc":"2.0","method":"notify.test"})";
    auto f = frame(req);
    ::send(fd, f.data(), f.size(), 0);

    drain_until(server, [&] { return *called; }, 500);
    REQUIRE(*called);

    // No response should arrive. Set the socket non-blocking and
    // confirm recv returns EAGAIN / EWOULDBLOCK within a short window.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    char buf[16];
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    REQUIRE((r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));

    ::close(fd);
    server.stop();
}

TEST_CASE("RpcServer sweeps stale sibling sockets on start",
          "[rpc_server]") {
    // Create a stale socket file (not bound to any listener) in the
    // same temp dir the sweep scans (vedit::temp_dir()). The sweep in
    // start() should unlink it.
    std::string stale_path =
        (vedit::temp_dir() / ("vedit-stale-"
         + std::to_string(::getpid()) + "-"
         + std::to_string(reinterpret_cast<std::uintptr_t>(&stale_path))
         + ".sock")).string();
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, stale_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    // bind only -- never listen. connect() will return ECONNREFUSED,
    // which the sweep treats as stale.
    REQUIRE(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) == 0);
    ::close(fd);
    struct stat st;
    REQUIRE(::stat(stale_path.c_str(), &st) == 0);

    vedit::rpc::Dispatcher d;
    std::string my_path = make_socket_path();
    vedit::rpc::RpcServer server(my_path, d);
    server.start();

    // The stale socket should now be gone. (My own socket will still
    // exist because it's live.)
    REQUIRE(::stat(stale_path.c_str(), &st) == -1);
    REQUIRE(errno == ENOENT);

    server.stop();
}

TEST_CASE("RpcServer drain returns count of dispatched requests",
          "[rpc_server]") {
    vedit::rpc::Dispatcher d;
    d.register_method("test.echo", [](const json&) {
        return json{{"ok", true}};
    });
    std::string path = make_socket_path();
    vedit::rpc::RpcServer server(path, d);
    server.start();

    int fd = connect_client(path);
    REQUIRE(fd >= 0);

    // Send one request; poll drain() until it reports the request was
    // dispatched. The I/O thread reads frames sequentially and queues
    // each before blocking on its future, so a single in-flight
    // request is the supported shape. drain() must return 1 once the
    // request is queued and dispatched.
    std::string req = R"({"jsonrpc":"2.0","method":"test.echo","id":1})";
    auto framed = frame(req);
    REQUIRE(::send(fd, framed.data(), framed.size(), 0)
            == static_cast<ssize_t>(framed.size()));

    // Poll drain() directly (not via drain_until, which would double-count
    // its own internal drain() call against this counter).
    std::size_t dispatched = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatched += server.drain();
        if (dispatched >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(dispatched == 1);

    // Read the response so the I/O thread's write completes and the
    // session tears down cleanly on stop().
    drain_until(server, [&] { return false; }, 100);
    std::string resp = recv_frame(fd);
    REQUIRE_FALSE(resp.empty());

    ::close(fd);
    server.stop();
}
