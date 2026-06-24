#ifndef VEDIT_RPC_METHODS_H
#define VEDIT_RPC_METHODS_H

namespace vedit {
class AppController;
namespace rpc {
class Dispatcher;
} // namespace rpc
} // namespace vedit

namespace vedit::rpc {

// Register all M1 JSON-RPC method handlers on the dispatcher.
// Each method is a thin wrapper around AppController.
void register_methods(Dispatcher& d, AppController& app);

} // namespace vedit::rpc

#endif
