add_executable(vedit main.cpp)
target_link_libraries(vedit PRIVATE
    vedit_rpc
    vedit_domain
    vedit_core
    vedit_util
    vedit_imgui_backends
    nfd::nfd
    ${VEDIT_SDL2_TARGET}
)

# On Windows, SDL2 #defines main to SDL_main via <SDL_main.h>. Linking
# SDL2main provides the WinMain entry point that calls SDL_main; without
# it the linker cannot find the CRT's `main` symbol.
if(WIN32 AND MSVC)
    find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(vedit PRIVATE SDL2::SDL2main)
endif()

if(APPLE)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    target_link_libraries(vedit PRIVATE ${APPKIT_LIBRARY} ${FOUNDATION_LIBRARY})

    set(VEDIT_ICON "${CMAKE_SOURCE_DIR}/resource/vedit.icns")
    target_sources(vedit PRIVATE "${VEDIT_ICON}")
    set_source_files_properties("${VEDIT_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    set_target_properties(vedit PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER com.vedit.app
        MACOSX_BUNDLE_BUNDLE_NAME vedit
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE vedit.icns
    )

    add_custom_target(vedit_package
        COMMAND "${CMAKE_COMMAND}" -E echo
                "Bundling dependencies into vedit.app..."
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/macos_bundle_deps.sh"
                "$<TARGET_BUNDLE_DIR:vedit>"
        DEPENDS vedit
        VERBATIM
        USES_TERMINAL
        COMMENT "Packaging vedit.app as a self-contained bundle"
    )
endif()
