add_library(vedit_domain STATIC
    project_model.cpp
    media_library.cpp
    timeline_model.cpp
)
target_include_directories(vedit_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vedit_domain PUBLIC vedit_core)
