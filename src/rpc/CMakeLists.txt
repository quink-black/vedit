add_library(vedit_rpc STATIC
    rpc_dispatcher.cpp
    rpc_server.cpp
    rpc_methods.cpp
)
target_include_directories(vedit_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vedit_rpc PUBLIC vedit_domain vedit_util nlohmann_json::nlohmann_json)
# Asio is private: only the .cpp implementations need its headers; the
# public interface uses Pimpl to keep Asio types out of headers.
target_link_libraries(vedit_rpc PRIVATE asio::asio)
