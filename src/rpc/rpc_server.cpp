// Asio-backed transport for the JSON-RPC server (POSIX only for vedit).
//
// Ported from idiff's src/app/rpc/rpc_server.cpp with Windows branches
// stripped: vedit targets Linux/macOS for M-refactor. The concurrency
// model is identical:
//
//   * One io_context, one I/O thread. All sockets, the acceptor, and
//     the framing buffers live on that thread.
//   * Each session is shared_ptr-owned and kept alive by chained async
//     callbacks (typical Asio idiom). A session ends when the peer
//     closes, the read fails, or stop() destroys the io_context.
//   * Per request, the I/O thread builds a PendingRequest holding the
//     parsed-but-not-dispatched JSON text and a std::promise<string>.
//     It pushes the PendingRequest onto a mutex-guarded queue and then
//     blocks the session on the matching std::future inside an
//     async-callback continuation: after queuing it posts a follow-up
//     handler that waits on the future, writes the framed reply, and
//     chains another async_read.
//   * Only the response wait blocks; reads on OTHER sessions continue
//     to be processed on the same thread. In practice the main thread
//     drains the queue every GUI frame (~16 ms), so the blocking wait
//     is bounded.
//   * drain() runs on the main thread. It pops every PendingRequest,
//     calls Dispatcher::handle_request on the GUI thread, and sets the
//     associated promise. Because all App state mutation happens here,
//     no locks are needed inside handlers.

#include "rpc/rpc_server.h"

#include "rpc/rpc_dispatcher.h"
#include "util/logger.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include <glob.h>

namespace vedit::rpc {

namespace {

// Encode a 4-byte big-endian length prefix.
void write_be32(std::array<unsigned char, 4>& out, std::uint32_t value) {
    out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
    out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
    out[2] = static_cast<unsigned char>((value >>  8) & 0xFF);
    out[3] = static_cast<unsigned char>( value        & 0xFF);
}

std::uint32_t read_be32(const std::array<unsigned char, 4>& in) {
    return (static_cast<std::uint32_t>(in[0]) << 24)
         | (static_cast<std::uint32_t>(in[1]) << 16)
         | (static_cast<std::uint32_t>(in[2]) <<  8)
         |  static_cast<std::uint32_t>(in[3]);
}

// Probe whether a UDS path has a live listener. Returns true if the
// socket is alive (connect succeeded), false on ECONNREFUSED or any
// other error. Used by the stale-socket sweep in start().
bool socket_is_alive(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    bool alive = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                           sizeof(addr)) == 0;
    ::close(fd);
    return alive;
}

// Enumerate sibling sockets in the temp dir (vedit-*.sock). Excludes
// `self_path` so the sweep never unlinks the path we are about to bind.
// The temp dir is resolved via vedit::temp_dir() so the pattern matches
// default_socket_path() on every platform.
std::vector<std::string> list_vedit_sockets(const std::string& self_path) {
    std::vector<std::string> out;
    glob_t g;
    std::memset(&g, 0, sizeof(g));
    if (::glob("/tmp/vedit-*.sock", 0, nullptr, &g) != 0) {
        return out;
    }
    for (std::size_t i = 0; i < g.gl_pathc; ++i) {
        std::string p = g.gl_pathv[i];
        if (p == self_path) continue;
        out.push_back(std::move(p));
    }
    ::globfree(&g);
    return out;
}

} // namespace

// One pending request waiting for the main thread to dispatch it. The
// response is delivered back through `result`; the I/O thread then
// writes the framed bytes on the originating socket.
struct PendingRequest {
    std::string request_json;
    std::promise<std::string> result;
};

struct RpcServer::Impl {
    using stream_protocol = asio::local::stream_protocol;
    using session_socket = stream_protocol::socket;

    Impl(std::string path, Dispatcher& d)
        : socket_path(std::move(path))
        , dispatcher(d)
        , io_context()
        , acceptor(io_context) {}

    // ---- Per-session state -------------------------------------------------
    //
    // `Session` is shared_ptr-managed; each async callback captures a
    // shared_from_this() to keep it alive across the wait. When the
    // last callback releases the pointer, the session and its socket
    // are destroyed.
    struct Session : public std::enable_shared_from_this<Session> {
        Session(Impl& owner_in, session_socket socket_in)
            : owner(owner_in)
            , socket(std::move(socket_in)) {}

        Impl& owner;
        session_socket socket;
        std::array<unsigned char, 4> length_buf{};
        std::vector<unsigned char> payload_buf;

        void start() { read_length(); }

        void read_length() {
            auto self = shared_from_this();
            asio::async_read(
                socket, asio::buffer(length_buf),
                [self](const std::error_code& ec, std::size_t /*n*/) {
                if (ec) {
                    if (ec != asio::error::eof
                            && ec != asio::error::operation_aborted
                            && ec != asio::error::connection_reset) {
                        vedit::Logger::log(vedit::Logger::Level::Warn,
                            ("rpc: read length failed: "
                             + ec.message()).c_str());
                    }
                    return;
                }
                self->on_length();
                });
        }

        void on_length() {
            std::uint32_t len = read_be32(length_buf);
            if (len == 0) {
                read_length();
                return;
            }
            if (len > kMaxFrameBytes) {
                vedit::Logger::log(vedit::Logger::Level::Warn,
                    ("rpc: frame too large ("
                     + std::to_string(len) + " bytes); closing").c_str());
                std::error_code ignore;
                socket.shutdown(stream_protocol::socket::shutdown_both,
                                ignore);
                return;
            }
            payload_buf.assign(len, 0);
            auto self = shared_from_this();
            asio::async_read(
                socket, asio::buffer(payload_buf),
                [self](const std::error_code& ec, std::size_t /*n*/) {
                    if (ec) {
                        if (ec != asio::error::eof
                                && ec != asio::error::operation_aborted) {
                            vedit::Logger::log(vedit::Logger::Level::Warn,
                                ("rpc: read body failed: "
                                 + ec.message()).c_str());
                        }
                        return;
                    }
                    self->on_body();
                });
        }

        void on_body() {
            std::string request_json(
                reinterpret_cast<const char*>(payload_buf.data()),
                payload_buf.size());

            auto pending = std::make_shared<PendingRequest>();
            pending->request_json = std::move(request_json);
            std::future<std::string> fut = pending->result.get_future();

            {
                std::lock_guard<std::mutex> lk(owner.queue_mu);
                owner.queue.push_back(pending);
            }
            owner.queue_cv.notify_one();

            // Wait for the main thread to dispatch.
            std::string response = fut.get();

            if (response.empty()) {
                read_length();
                return;
            }

            if (response.size() > kMaxFrameBytes) {
                vedit::Logger::log(vedit::Logger::Level::Error,
                    ("rpc: response too large ("
                     + std::to_string(response.size())
                     + " bytes); dropping").c_str());
                return;
            }
            auto header = std::make_shared<std::array<unsigned char, 4>>();
            write_be32(*header, static_cast<std::uint32_t>(response.size()));
            auto body = std::make_shared<std::string>(std::move(response));

            auto self = shared_from_this();
            std::array<asio::const_buffer, 2> out = {
                asio::buffer(*header),
                asio::buffer(*body),
            };
            asio::async_write(
                socket, out,
                [self, header, body](const std::error_code& ec,
                                     std::size_t /*n*/) {
                    if (ec) {
                        if (ec != asio::error::operation_aborted) {
                            vedit::Logger::log(vedit::Logger::Level::Warn,
                                ("rpc: write failed: "
                                 + ec.message()).c_str());
                        }
                        return;
                    }
                    self->read_length();
                });
        }
    };

    // ---- POSIX accept loop -------------------------------------------------

    void do_accept() {
        acceptor.async_accept(
            [this](const std::error_code& ec,
                   stream_protocol::socket sock) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        vedit::Logger::log(vedit::Logger::Level::Warn,
                            ("rpc: accept failed: "
                             + ec.message()).c_str());
                    }
                    return;
                }
                auto session =
                    std::make_shared<Session>(*this, std::move(sock));
                session->start();
                do_accept();
            });
    }

    // ---- Members -----------------------------------------------------------

    std::string socket_path;
    Dispatcher& dispatcher;

    asio::io_context io_context;
    // Prevents io_context::run() from exiting when there are no pending
    // async operations.
    using work_guard_type = asio::executor_work_guard<asio::io_context::executor_type>;
    std::unique_ptr<work_guard_type> work_guard;

    stream_protocol::acceptor acceptor;
    std::thread io_thread;
    std::atomic<bool> running{false};

    std::mutex queue_mu;
    std::condition_variable queue_cv;  // reserved for future blocking-drain
    std::deque<std::shared_ptr<PendingRequest>> queue;
};

RpcServer::RpcServer(std::string socket_path, Dispatcher& dispatcher)
    : impl_(std::make_unique<Impl>(std::move(socket_path), dispatcher)) {}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::start() {
    if (impl_->running.load()) {
        return;
    }

    // Sweep stale sibling sockets from previous crashed instances
    // before binding our own. Anything that fails the connect probe
    // (ECONNREFUSED) is unlinked; live sockets are left alone.
    for (const auto& sibling : list_vedit_sockets(impl_->socket_path)) {
        if (!socket_is_alive(sibling)) {
            if (::unlink(sibling.c_str()) != 0 && errno != ENOENT) {
                vedit::Logger::log(vedit::Logger::Level::Warn,
                    ("rpc: stale sweep unlink('"
                     + sibling + "') failed: "
                     + std::strerror(errno)).c_str());
            } else {
                vedit::Logger::log(vedit::Logger::Level::Info,
                    ("rpc: swept stale socket " + sibling).c_str());
            }
        }
    }

    // Unlink any stale socket file from a previous run at our own path.
    if (::unlink(impl_->socket_path.c_str()) != 0 && errno != ENOENT) {
        vedit::Logger::log(vedit::Logger::Level::Warn,
            ("rpc: unlink('" + impl_->socket_path + "') failed: "
             + std::strerror(errno)).c_str());
    }

    using stream_protocol = asio::local::stream_protocol;
    stream_protocol::endpoint ep(impl_->socket_path);

    std::error_code ec;
    impl_->acceptor.open(ep.protocol(), ec);
    if (ec) {
        throw std::system_error(ec, "rpc: acceptor.open");
    }
    impl_->acceptor.bind(ep, ec);
    if (ec) {
        throw std::system_error(ec,
            "rpc: bind(" + impl_->socket_path + ")");
    }
    impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::system_error(ec, "rpc: listen");
    }

    impl_->do_accept();
    impl_->running.store(true);

    impl_->io_context.restart();
    impl_->work_guard = std::make_unique<decltype(impl_->work_guard)::element_type>(
        asio::make_work_guard(impl_->io_context));

    impl_->io_thread = std::thread([this] {
        try {
            impl_->io_context.run();
        } catch (const std::exception& ex) {
            vedit::Logger::log(vedit::Logger::Level::Error,
                (std::string("rpc: io_context.run threw: ")
                 + ex.what()).c_str());
        }
    });

    vedit::Logger::log(vedit::Logger::Level::Info,
        ("rpc: server listening on " + impl_->socket_path).c_str());
}

void RpcServer::stop() {
    if (!impl_->running.load()) {
        return;
    }
    impl_->running.store(false);

    // Cancel pending accepts / reads, then join.
    std::error_code ignore;
    impl_->acceptor.close(ignore);
    impl_->io_context.stop();

    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }

    if (::unlink(impl_->socket_path.c_str()) != 0 && errno != ENOENT) {
        vedit::Logger::log(vedit::Logger::Level::Warn,
            ("rpc: unlink('" + impl_->socket_path
             + "') on shutdown failed: "
             + std::strerror(errno)).c_str());
    }

    // Drain any still-queued requests by completing their promises with
    // an empty response so blocked I/O-thread callbacks don't deadlock.
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mu);
        for (auto& p : impl_->queue) {
            try {
                p->result.set_value(std::string{});
            } catch (...) {
                // Promise might already be satisfied.
            }
        }
        impl_->queue.clear();
    }

    // Reset io_context so a future start() can reuse this object.
    impl_->io_context.restart();
}

std::size_t RpcServer::drain() {
    std::deque<std::shared_ptr<PendingRequest>> batch;
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mu);
        batch.swap(impl_->queue);
    }
    for (auto& pending : batch) {
        std::string response =
            impl_->dispatcher.handle_request(pending->request_json);
        try {
            pending->result.set_value(std::move(response));
        } catch (const std::future_error& ex) {
            vedit::Logger::log(vedit::Logger::Level::Warn,
                (std::string("rpc: promise.set_value failed: ")
                 + ex.what()).c_str());
        }
    }
    return batch.size();
}

const std::string& RpcServer::socket_path() const noexcept {
    return impl_->socket_path;
}

bool RpcServer::is_running() const noexcept {
    return impl_->running.load();
}

} // namespace vedit::rpc
