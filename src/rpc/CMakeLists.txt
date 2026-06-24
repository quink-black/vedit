add_library(vedit_rpc STATIC
    rpc_dispatcher.cpp
    rpc_server.cpp
    rpc_methods.cpp
)
target_include_directories(vedit_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vedit_rpc PUBLIC vedit_domain)
# nlohmann-json is header-only; find it via the system package.
find_package(nlohmann_json REQUIRED)
target_link_libraries(vedit_rpc PUBLIC nlohmann_json::nlohmann_json)
