add_library(vedit_core STATIC
    mlt_runtime.cpp
    preview_renderer.cpp
)
target_include_directories(vedit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vedit_core PUBLIC vedit_util ${VEDIT_MLT_TARGET})
