#include <catch2/catch_test_macros.hpp>

#include "rpc/rpc_dispatcher.h"

using nlohmann::json;

TEST_CASE("Dispatcher returns error for non-object request", "[rpc]") {
    vedit::rpc::Dispatcher d;
    std::string response = d.handle_request("42");
    auto parsed = json::parse(response);
    REQUIRE(parsed.contains("error"));
    REQUIRE(parsed["error"]["code"] == -32600);
}

TEST_CASE("Dispatcher returns error for missing method", "[rpc]") {
    vedit::rpc::Dispatcher d;
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"nonexistent","id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed["error"]["code"] == -32601);
}

TEST_CASE("Dispatcher calls registered handler and returns result", "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("test.echo", [](const json& params) {
        return json{{"received", params}};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"test.echo","params":{"x":1},"id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed.contains("result"));
    REQUIRE(parsed["result"]["received"]["x"] == 1);
}

TEST_CASE("Dispatcher handles batch requests", "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("a", [](const json&) { return json{{"ok", true}}; });
    d.register_method("b", [](const json&) { return json{{"ok", true}}; });
    std::string response = d.handle_request(
        R"([{"jsonrpc":"2.0","method":"a","id":1},{"jsonrpc":"2.0","method":"b","id":2}])");
    auto parsed = json::parse(response);
    REQUIRE(parsed.is_array());
    REQUIRE(parsed.size() == 2);
}

TEST_CASE("Dispatcher treats missing id as notification", "[rpc]") {
    vedit::rpc::Dispatcher d;
    auto called = std::make_shared<bool>(false);
    d.register_method("notify", [called](const json&) {
        *called = true;
        return json{{"ok", true}};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"notify"})");
    REQUIRE(*called);
    REQUIRE(response.empty());  // no response for notification
}

TEST_CASE("RpcException produces structured error", "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("fail", [](const json&) -> json {
        throw vedit::rpc::RpcException(vedit::rpc::ErrorCode::ApplicationError,
                                       "something broke",
                                       json{{"detail", "info"}});
        return json{};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"fail","id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed["error"]["code"] == -32000);
    REQUIRE(parsed["error"]["message"] == "something broke");
    REQUIRE(parsed["error"]["data"]["detail"] == "info");
}

TEST_CASE("Uncaught exception returns InternalError", "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("crash", [](const json&) -> json {
        throw std::runtime_error("oops");
        return json{};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"crash","id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed["error"]["code"] == -32603);
    REQUIRE(parsed["error"]["message"].get<std::string>().find("oops") != std::string::npos);
}

// Handlers read params with value()/at(); a type mismatch used to leak
// out as nlohmann's exception and be reported as InternalError, which
// tells an agent the server is broken rather than that its request was
// malformed.
TEST_CASE("Type mismatch in params reports InvalidParams, not InternalError",
          "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("needs.number", [](const json& params) {
        return json{{"value", params.value("n", 0LL)}};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"needs.number","params":{"n":"abc"},"id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed.contains("error"));
    REQUIRE(parsed["error"]["code"] == -32602);
}

TEST_CASE("Missing params object still reaches the handler", "[rpc]") {
    vedit::rpc::Dispatcher d;
    d.register_method("needs.number", [](const json& params) {
        return json{{"value", params.value("n", 0LL)}};
    });
    std::string response = d.handle_request(
        R"({"jsonrpc":"2.0","method":"needs.number","id":1})");
    auto parsed = json::parse(response);
    REQUIRE(parsed.contains("result"));
    REQUIRE(parsed["result"]["value"] == 0);
}
