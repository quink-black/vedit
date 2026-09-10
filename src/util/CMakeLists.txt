add_library(vedit_util STATIC
    logger.cpp
    waveform.cpp
)
target_include_directories(vedit_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
