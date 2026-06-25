# ImGui is built from local source (third_party/imgui/) so no system
# pkg-config dependency is needed. This approach works on both Linux and
# macOS. The local source includes core files, backends, and imconfig.h.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

add_library(vedit_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(vedit_imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(vedit_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(vedit_imgui PUBLIC PkgConfig::SDL2)

# Alias for backward compatibility with existing targets
add_library(vedit_imgui_backends ALIAS vedit_imgui)

# ---- Native File Dialog Extended (NFD) ----
# System-native file open/save dialogs. The find_package() call lives in
# the top-level CMakeLists.txt so the resulting IMPORTED target is visible
# from every subdirectory; here we only need to pull the source in when
# the system package was absent.
if(VEDIT_BUILD_NFD)
    include(FetchContent)
    FetchContent_Declare(
        nativefiledialog-extended
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nativefiledialog-extended)
endif()
