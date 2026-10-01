#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

namespace {

// The keys of a tool's inputSchema properties block. A declaration runs from
// `name="<tool>"` to the next `Tool(`, and the block is delimited by counting
// braces from the `{` that follows `"properties":`; the keys inside it are the
// parameters the bridge is expected to forward.
std::map<std::string, std::set<std::string>> tool_parameters(
    const std::string& path) {
    std::map<std::string, std::set<std::string>> found;
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    size_t cursor = 0;
    while (true) {
        size_t at = text.find("name=\"", cursor);
        if (at == std::string::npos) break;
        at += 6;
        size_t end = text.find('"', at);
        if (end == std::string::npos) break;
        const std::string tool = text.substr(at, end - at);
        const size_t next = text.find("Tool(", end);
        const std::string body =
            text.substr(end, next == std::string::npos ? std::string::npos
                                                       : next - end);
        cursor = next == std::string::npos ? text.size() : next;

        const size_t props = body.find("\"properties\"");
        if (props == std::string::npos) {
            found[tool];  // declares no parameters
            continue;
        }
        const size_t open = body.find('{', props + 11);
        if (open == std::string::npos) continue;

        std::set<std::string> names;
        int depth = 0;
        for (size_t i = open; i < body.size(); ++i) {
            if (body[i] == '{') {
                ++depth;
                continue;
            }
            if (body[i] == '}') {
                --depth;
                if (depth == 0) break;
                continue;
            }
            if (depth != 1 || body[i] != '"') continue;
            const size_t key_end = body.find('"', i + 1);
            if (key_end == std::string::npos) break;
            size_t after = key_end + 1;
            while (after < body.size() &&
                   (body[after] == ' ' || body[after] == '\n'))
                ++after;
            if (after < body.size() && body[after] == ':')
                names.insert(body.substr(i + 1, key_end - i - 1));
            i = key_end;
        }
        found[tool] = names;
    }
    return found;
}

// The bridge's dispatch code split by tool: each branch runs from
// `if name == "<tool>":` to the next such line.
std::map<std::string, std::string> tool_branches(const std::string& path) {
    const std::string marker = "if name == \"";
    std::map<std::string, std::string> found;
    std::ifstream in(path);
    std::string line;
    std::string tool;
    std::string body;
    while (std::getline(in, line)) {
        size_t at = line.find(marker);
        // Only a line that opens a dispatch branch counts: an expression such
        // as `x = "undo" if name == "undo" else ...` mentions the same marker
        // and would cut the branch in the middle.
        if (at != std::string::npos && line.find_first_not_of(" \t") != at)
            at = std::string::npos;
        if (at != std::string::npos) {
            if (!tool.empty()) found[tool] = body;
            at += marker.size();
            const size_t end = line.find('"', at);
            tool = end == std::string::npos ? std::string()
                                            : line.substr(at, end - at);
            body.clear();
        }
        if (!tool.empty()) body += line + "\n";
    }
    if (!tool.empty()) found[tool] = body;
    return found;
}

}  // namespace

TEST_CASE("every MCP tool parameter reaches the RPC call", "[docs][mcp]") {
    const std::string file =
        std::string(VEDIT_SOURCE_DIR) + "/tools/vedit-mcp/vedit_mcp_server.py";
    const auto parameters = tool_parameters(file);
    const auto branches = tool_branches(file);
    // A parse that finds nothing would let every tool pass by having nothing
    // to check.
    REQUIRE(parameters.size() > 20);
    REQUIRE(branches.size() > 20);

    std::string problems;
    for (const auto& tool : parameters) {
        const auto branch = branches.find(tool.first);
        if (branch == branches.end()) {
            problems += "\n  no dispatch branch for tool: " + tool.first;
            continue;
        }
        for (const auto& parameter : tool.second) {
            if (branch->second.find("\"" + parameter + "\"") ==
                std::string::npos)
                problems += "\n  " + tool.first + " declares " + parameter +
                            " but never forwards it";
        }
    }
    if (!problems.empty())
        FAIL("MCP tool declarations and the bridge disagree:" + problems);
}

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
