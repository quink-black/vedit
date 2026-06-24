add_executable(vedit main.cpp)
target_link_libraries(vedit PRIVATE
    vedit_rpc
    vedit_domain
    vedit_core
    vedit_util
    vedit_imgui_backends
    nfd::nfd
    PkgConfig::SDL2
)
# OpenGL for the GL3 backend
target_link_libraries(vedit PRIVATE GL EGL)
