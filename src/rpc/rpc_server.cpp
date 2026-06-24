#include "rpc_server.h"

#include "util/logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace vedit::rpc {

namespace {

constexpr size_t kMaxFrameSize = 64 * 1024 * 1024;  // 64 MiB

} // namespace

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::start(RequestHandler handler) {
    if (running_) return true;
    handler_ = std::move(handler);

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Logger::log(Logger::Level::Error, "RpcServer: socket() failed");
        return false;
    }

    // Non-blocking so we can accept + drain without blocking the I/O
    // thread. We still use blocking send/recv on the client fds.
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    socket_path_ = "/tmp/vedit-" + std::to_string(getpid()) + ".sock";
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    unlink(socket_path_.c_str());
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        Logger::log(Logger::Level::Error, "RpcServer: bind failed");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // Single client at a time is fine for M1.
    if (listen(listen_fd_, 1) < 0) {
        Logger::log(Logger::Level::Error, "RpcServer: listen failed");
        close(listen_fd_);
        unlink(socket_path_.c_str());
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    io_thread_ = std::thread(&RpcServer::io_loop, this);
    Logger::log(Logger::Level::Info, "RpcServer: listening");
    return true;
}

void RpcServer::drain() {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        pending.swap(pending_requests_);
    }
    for (const auto& req : pending) {
        std::string response = handler_(req);
        // For M1, we don't write responses back (single-direction
        // request model). The response is discarded here; a full
        // JSON-RPC transport with responses would write to the client
        // fd, but that requires per-connection tracking. M1 accepts
        // the simpler contract: the caller can verify state via
        // subsequent state.get calls.
        (void)response;
    }
}

void RpcServer::stop() {
    if (!running_) return;
    running_ = false;
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(socket_path_.c_str());
        listen_fd_ = -1;
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    Logger::log(Logger::Level::Info, "RpcServer: stopped");
}

void RpcServer::io_loop() {
    while (running_) {
        // Accept a connection (non-blocking).
        int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            usleep(10000);  // 10ms backoff
            continue;
        }

        // Read length-prefixed frames from this client.
        while (running_) {
            uint32_t len_be = 0;
            ssize_t n = recv(client, &len_be, 4, MSG_WAITALL);
            if (n <= 0) break;  // client disconnected
            uint32_t len = ntohl(len_be);
            if (len == 0 || len > kMaxFrameSize) break;

            std::string frame(len, '\0');
            n = recv(client, frame.data(), len, MSG_WAITALL);
            if (n != static_cast<ssize_t>(len)) break;

            {
                std::lock_guard<std::mutex> lk(queue_mutex_);
                pending_requests_.push_back(std::move(frame));
            }
        }
        close(client);
    }
}

} // namespace vedit::rpc
