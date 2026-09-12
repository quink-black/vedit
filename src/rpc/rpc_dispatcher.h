#ifndef VEDIT_RPC_DISPATCHER_H
#define VEDIT_RPC_DISPATCHER_H

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace vedit::rpc {

enum class ErrorCode : int {
    ParseError       = -32700,
    InvalidRequest   = -32600,
    MethodNotFound   = -32601,
    InvalidParams    = -32602,
    InternalError    = -32603,
    ApplicationError = -32000,
};

class RpcException : public std::runtime_error {
public:
    RpcException(ErrorCode code, std::string msg,
                 nlohmann::json data = nullptr)
        : std::runtime_error(msg), code_(code), msg_(std::move(msg)),
          data_(std::move(data)) {}
    ErrorCode code() const noexcept { return code_; }
    const std::string& msg() const noexcept { return msg_; }
    const nlohmann::json& data() const noexcept { return data_; }
private:
    ErrorCode code_;
    std::string msg_;
    nlohmann::json data_;
};

using MethodHandler =
    std::function<nlohmann::json(const nlohmann::json& params)>;

class Dispatcher {
public:
    void register_method(std::string name, MethodHandler handler);
    bool has_method(const std::string& name) const noexcept;
    // Every registered name, sorted. Lets tests compare the dispatcher
    // against documented tables so the docs cannot drift from the code.
    std::vector<std::string> method_names() const;
    std::string handle_request(const std::string& request_json);
private:
    nlohmann::json handle_one(const nlohmann::json& request,
                              bool& is_notification) noexcept;
    std::unordered_map<std::string, MethodHandler> methods_;
};

nlohmann::json make_error_response(ErrorCode code,
                                   const std::string& message,
                                   const nlohmann::json& id = nullptr,
                                   const nlohmann::json& data = nullptr);

} // namespace vedit::rpc

#endif
