add_library(vedit_domain STATIC
    project_model.cpp
    media_library.cpp
    timeline_model.cpp
    player_controller.cpp
    app_controller.cpp
    export_service.cpp
    project_save_load.cpp
    filter_catalog.cpp
    text_filter.cpp
)
target_include_directories(vedit_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vedit_domain PUBLIC vedit_core nlohmann_json::nlohmann_json)

# Core Text provides the glyph rasterizer for the in-process text filter.
if(APPLE)
    find_library(CORE_FOUNDATION_LIBRARY CoreFoundation)
    find_library(CORE_TEXT_LIBRARY CoreText)
    find_library(CORE_GRAPHICS_LIBRARY CoreGraphics)
    find_library(APP_SERVICES_LIBRARY ApplicationServices)
    target_link_libraries(vedit_domain PUBLIC
        ${CORE_FOUNDATION_LIBRARY}
        ${CORE_TEXT_LIBRARY}
        ${CORE_GRAPHICS_LIBRARY}
        ${APP_SERVICES_LIBRARY}
    )
endif()
