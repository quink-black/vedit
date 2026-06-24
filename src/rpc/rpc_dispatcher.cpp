#include "rpc_dispatcher.h"

#include "util/logger.h"

#include <utility>

namespace vedit::rpc {

using nlohmann::json;

namespace {

struct IdSlot { bool present = false; json value; };

IdSlot extract_id(const json& request) {
    IdSlot slot;
    auto it = request.find("id");
    if (it != request.end()) { slot.present = true; slot.value = *it; }
    return slot;
}

bool is_valid_id(const json& id) {
    return id.is_null() || id.is_string() || id.is_number();
}

} // namespace

json make_error_response(ErrorCode code, const std::string& message,
                         const json& id, const json& data) {
    json error_obj = {{"code", static_cast<int>(code)}, {"message", message}};
    if (!data.is_null()) error_obj["data"] = data;
    return json{{"jsonrpc", "2.0"}, {"error", std::move(error_obj)}, {"id", id}};
}

void Dispatcher::register_method(std::string name, MethodHandler handler) {
    methods_[std::move(name)] = std::move(handler);
}

bool Dispatcher::has_method(const std::string& name) const noexcept {
    return methods_.find(name) != methods_.end();
}

json Dispatcher::handle_one(const json& request, bool& is_notification) noexcept {
    is_notification = false;
    if (!request.is_object())
        return make_error_response(ErrorCode::InvalidRequest, "Request must be a JSON object", nullptr);

    IdSlot id_slot = extract_id(request);
    if (id_slot.present && !is_valid_id(id_slot.value))
        return make_error_response(ErrorCode::InvalidRequest, "Request id must be string, number, or null", nullptr);
    is_notification = !id_slot.present;
    json response_id = id_slot.present ? id_slot.value : json(nullptr);

    auto version_it = request.find("jsonrpc");
    if (version_it == request.end() || !version_it->is_string() || version_it->get<std::string>() != "2.0")
        return make_error_response(ErrorCode::InvalidRequest, "Missing or unsupported jsonrpc version", response_id);

    auto method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string() || method_it->get<std::string>().empty())
        return make_error_response(ErrorCode::InvalidRequest, "Missing or invalid method name", response_id);
    const std::string method_name = method_it->get<std::string>();

    json params = json(nullptr);
    auto params_it = request.find("params");
    if (params_it != request.end()) {
        if (!params_it->is_object() && !params_it->is_array())
            return make_error_response(ErrorCode::InvalidParams, "params must be an object or array", response_id);
        params = *params_it;
    }

    auto handler_it = methods_.find(method_name);
    if (handler_it == methods_.end())
        return make_error_response(ErrorCode::MethodNotFound, "Method not found: " + method_name, response_id);

    try {
        json result = handler_it->second(params);
        return json{{"jsonrpc", "2.0"}, {"result", std::move(result)}, {"id", response_id}};
    } catch (const RpcException& ex) {
        return make_error_response(ex.code(), ex.msg(), response_id, ex.data());
    } catch (const std::exception& ex) {
        return make_error_response(ErrorCode::InternalError, std::string("Internal error: ") + ex.what(), response_id);
    } catch (...) {
        return make_error_response(ErrorCode::InternalError, "Internal error: unknown exception", response_id);
    }
}

std::string Dispatcher::handle_request(const std::string& request_json) {
    json parsed;
    try {
        parsed = json::parse(request_json);
    } catch (const json::parse_error& ex) {
        return make_error_response(ErrorCode::ParseError, std::string("Parse error: ") + ex.what(), nullptr).dump();
    }

    if (parsed.is_object()) {
        bool is_notification = false;
        json response = handle_one(parsed, is_notification);
        return is_notification ? std::string{} : response.dump();
    }

    if (parsed.is_array()) {
        if (parsed.empty())
            return make_error_response(ErrorCode::InvalidRequest, "Batch request must not be empty", nullptr).dump();
        json batch_response = json::array();
        for (const auto& item : parsed) {
            bool is_notification = false;
            json response = handle_one(item, is_notification);
            if (!is_notification) batch_response.push_back(std::move(response));
        }
        return batch_response.empty() ? std::string{} : batch_response.dump();
    }

    return make_error_response(ErrorCode::InvalidRequest, "Top-level JSON must be an object or array", nullptr).dump();
}

} // namespace vedit::rpc
