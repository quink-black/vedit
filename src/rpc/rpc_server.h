#ifndef VEDIT_RPC_SERVER_H
#define VEDIT_RPC_SERVER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vedit::rpc {

// Lightweight JSON-RPC 2.0 transport over Unix domain sockets.
// M1 simplification of idiff's RpcServer: the I/O thread pushes
// incoming request frames into a queue; the main thread drains
// them via drain().
//
// Invariants:
//   - The I/O thread never touches AppController / MLT / OpenGL.
//   - drain() is called from the main thread; handlers run there.
//   - Wire format: 4-byte big-endian length prefix, then JSON.
//     Max 64 MiB per frame.
class RpcServer {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;

    RpcServer();
    ~RpcServer();

    // Start listening on /tmp/vedit-<pid>.sock. Returns false on error.
    bool start(RequestHandler handler);

    // Drain pending requests on the main thread. Call once per frame.
    void drain();

    // Stop the server and join the I/O thread.
    void stop();

    bool running() const { return running_; }

private:
    void io_loop();

    RequestHandler handler_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::string socket_path_;

    // Pending request frames pushed by the I/O thread.
    std::mutex queue_mutex_;
    std::vector<std::string> pending_requests_;
};

} // namespace vedit::rpc

#endif
