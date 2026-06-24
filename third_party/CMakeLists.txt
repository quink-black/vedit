# ImGui core comes from the apt libimgui-dev (libimgui.a + imgui pkg).
# The apt package ships the platform backend .cpp files under
# /usr/share/doc/libimgui-dev/examples/backends/ rather than in the
# include dir, so we compile those two into a thin static lib here.

find_package(PkgConfig REQUIRED)
pkg_check_modules(IMGUI REQUIRED IMPORTED_TARGET imgui)

set(IMGUI_BACKEND_DIR "/usr/share/doc/libimgui-dev/examples/backends")
set(IMGUI_INCLUDE_DIR  "/usr/include/imgui/backends")

add_library(vedit_imgui_backends STATIC
    ${IMGUI_BACKEND_DIR}/imgui_impl_sdl2.cpp
    ${IMGUI_BACKEND_DIR}/imgui_impl_sdlrenderer2.cpp
)
target_link_libraries(vedit_imgui_backends PUBLIC
    PkgConfig::IMGUI
    PkgConfig::SDL2
)
target_include_directories(vedit_imgui_backends PUBLIC
    ${IMGUI_INCLUDE_DIR}
    /usr/include/imgui
)
target_compile_definitions(vedit_imgui_backends PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

# ---- Native File Dialog Extended (NFD) ----
# System-native file open/save dialogs. Try the system package first
# (matches idiff's pattern); fall back to building from source via
# FetchContent so a fresh checkout always works.
find_package(nfd QUIET)
if(NOT nfd_FOUND)
    message(STATUS "nfd not found on system -- will build from source")
    include(FetchContent)
    FetchContent_Declare(
        nativefiledialog-extended
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nativefiledialog-extended)
else()
    message(STATUS "Found system nfd: ${nfd_DIR}")
endif()
