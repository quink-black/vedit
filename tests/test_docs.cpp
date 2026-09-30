#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "domain/app_controller.h"
#include "rpc/rpc_dispatcher.h"
#include "rpc/rpc_methods.h"

namespace {

// The docs rotted because nothing compared them to the code: the README
// documented 16 of 57 methods and the MCP README 17 of 51 tools. These
// cases parse the registered handlers and the two documents so the next
// method added without a doc entry fails the build. The third case
// compares the same registry against the methods the MCP bridge can
// actually reach, so a method registered without a tool fails too.

std::set<std::string> documented_names(const std::string& path) {
    std::set<std::string> found;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        // Table rows start with "| `name`".
        if (line.size() < 4 || line[0] != '|' || line[2] != '`') continue;
        size_t end = line.find('`', 3);
        if (end == std::string::npos) continue;
        std::string name = line.substr(3, end - 3);
        // Method and tool names are dotted or underscored lowercase.
        bool ok = !name.empty();
        for (char c : name) {
            if (!(std::islower(static_cast<unsigned char>(c)) || c == '.' ||
                  c == '_'))
                ok = false;
        }
        if (ok) found.insert(name);
    }
    return found;
}

// The bridge is an agent's only route to the RPC surface, so the method
// names written into its _call(instance, "<name>", ...) round-trips are
// exactly the reachable set. Comparing those literals, rather than the
// tool list, catches a tool that was declared but never forwards, and a
// forward whose method no longer exists.
std::set<std::string> bridge_called_methods(const std::string& path) {
    const std::string marker = "_call(instance, \"";
    std::set<std::string> found;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        while ((pos = line.find(marker, pos)) != std::string::npos) {
            pos += marker.size();
            size_t end = line.find('"', pos);
            if (end == std::string::npos) break;
            found.insert(line.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    return found;
}

} // namespace

TEST_CASE("README documents every registered RPC method", "[docs]") {
    vedit::AppController app;
    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    const auto registered = d.method_names();
    REQUIRE_FALSE(registered.empty());
    const auto documented = documented_names(std::string(VEDIT_SOURCE_DIR) + "/README.md");

    std::vector<std::string> missing;
    for (const auto& name : registered) {
        if (documented.count(name) == 0) missing.push_back(name);
    }
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += " " + m;
        FAIL("undocumented RPC methods:" + list);
    }
}

TEST_CASE("MCP README documents every MCP tool", "[docs]") {
    std::set<std::string> tools;
    std::ifstream in(std::string(VEDIT_SOURCE_DIR) + "/tools/vedit-mcp/vedit_mcp_server.py");
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        while ((pos = line.find("name=\"", pos)) != std::string::npos) {
            pos += 6;
            size_t end = line.find('"', pos);
            if (end == std::string::npos) break;
            tools.insert(line.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    REQUIRE_FALSE(tools.empty());

    const auto documented =
        documented_names(std::string(VEDIT_SOURCE_DIR) + "/tools/vedit-mcp/README.md");
    std::vector<std::string> missing;
    for (const auto& tool : tools) {
        if (documented.count(tool) == 0) missing.push_back(tool);
    }
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += " " + m;
        FAIL("undocumented MCP tools:" + list);
    }
}

TEST_CASE("MCP bridge reaches every registered RPC method", "[docs][mcp]") {
    vedit::AppController app;
    vedit::rpc::Dispatcher d;
    vedit::rpc::register_methods(d, app);

    const auto registered = d.method_names();
    REQUIRE_FALSE(registered.empty());
    const std::set<std::string> registered_names(registered.begin(),
                                                 registered.end());

    const auto reached = bridge_called_methods(
        std::string(VEDIT_SOURCE_DIR) + "/tools/vedit-mcp/vedit_mcp_server.py");
    // Zero calls means the bridge file moved or stopped matching the
    // parsed shape, and an empty set would make either direction below
    // pass vacuously.
    REQUIRE_FALSE(reached.empty());

    std::string problems;
    for (const auto& name : registered) {
        if (reached.count(name) == 0)
            problems += "\n  registered but no MCP tool reaches it: " + name;
    }
    for (const auto& name : reached) {
        if (registered_names.count(name) == 0)
            problems += "\n  MCP bridge calls an unregistered method: " + name;
    }
    if (!problems.empty())
        FAIL("RPC methods and the MCP bridge disagree:" + problems);
}
