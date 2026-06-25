# Dear ImGui -- always built from source via FetchContent (no standard
# system package).  Mirrors idiff's approach: pull the docking branch from
# GitHub, compile the core + SDL2/SDL_Renderer backends into a static lib.
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

add_library(vedit_imgui STATIC ${IMGUI_SOURCES})
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
    FetchContent_Declare(
        nativefiledialog-extended
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nativefiledialog-extended)
endif()
