# Build MLT from source via ExternalProject_Add.
#
# MLT is not in the vcpkg registry and its upstream vcpkg.json pulls
# Qt/opencv/gdk-pixbuf, which vedit does not use. Instead, build MLT from
# source with only the four modules vedit needs (avformat, xml, sdl2,
# core framework), forwarding the vcpkg toolchain so MLT's find_package
# and pkg_check_modules calls resolve the vcpkg-installed dependencies.
#
# The resulting IMPORTED targets (Mlt7::mlt, Mlt7::mlt++) point at the
# future install location. The actual build order is enforced by the
# caller via add_dependencies(<vedit_target> mlt_external).

include(ExternalProject)

function(vedit_build_mlt)
    set(MLT_PREFIX  "${CMAKE_BINARY_DIR}/_mlt")
    set(MLT_INSTALL "${MLT_PREFIX}/install")

    # Enable only the modules vedit uses. Every ON-by-default module not
    # listed here as ON is explicitly OFF to keep the build minimal and
    # avoid pulling Qt/opencv/gdk-pixbuf/frei0r/rtaudio/movit/etc.
    set(MLT_MODULE_ARGS
        -DMOD_AVFORMAT=ON
        -DMOD_XML=ON
        -DMOD_SDL2=ON
        -DMOD_DECKLINK=OFF
        -DMOD_FREI0R=OFF
        -DMOD_GDK=OFF
        -DMOD_GLAXNIMATE_QT6=OFF
        -DMOD_JACKRACK=OFF
        -DMOD_KDENLIVE=OFF
        -DMOD_MOVIT=OFF
        -DMOD_NDI=OFF
        -DMOD_NORMALIZE=OFF
        -DMOD_OLDFILM=OFF
        -DMOD_OPENFX=OFF
        -DMOD_PLUS=OFF
        -DMOD_PLUSGPL=OFF
        -DMOD_QT6=OFF
        -DMOD_RESAMPLE=OFF
        -DMOD_RTAUDIO=OFF
        -DMOD_RUBBERBAND=OFF
        -DMOD_RNNOISE=OFF
        -DMOD_SDL1=OFF
        -DMOD_SOX=OFF
        -DMOD_SPATIALAUDIO=OFF
        -DMOD_VIDSTAB=OFF
        -DMOD_VORBIS=OFF
        -DMOD_XINE=OFF
        -DUSE_AVDEVICE=OFF
        -DUSE_LV2=OFF
        -DUSE_VST2=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_DOCS=OFF
    )

    set(MLT_CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${MLT_INSTALL}
        -DCMAKE_C_FLAGS=/utf-8
        -DCMAKE_CXX_FLAGS=/utf-8
        ${MLT_MODULE_ARGS}
    )

    # Forward the vcpkg toolchain so MLT finds ffmpeg/sdl2/glib/etc.
    # installed by vcpkg. Disable manifest mode for the inner build so
    # MLT's own vcpkg.json does not trigger a fresh dependency install
    # (the system install already has everything we need).
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND MLT_CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DVCPKG_MANIFEST_MODE=OFF)
    endif()
    if(VCPKG_TARGET_TRIPLET)
        list(APPEND MLT_CMAKE_ARGS
            -DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET})
    endif()

    # Only build MLT if it is not already installed. After the first
    # build incremental reconfigures skip the ExternalProject entirely,
    # avoiding the git-update + cmake-configure overhead (~10 s per
    # no-op build).
    set(_mlt_installed TRUE)
    foreach(_lib "lib/mlt-7.lib" "lib/mlt++-7.lib"
                 "mlt-7.dll" "mlt++-7.dll")
        if(NOT EXISTS "${MLT_INSTALL}/${_lib}")
            set(_mlt_installed FALSE)
            break()
        endif()
    endforeach()

    if(NOT _mlt_installed)
        ExternalProject_Add(mlt_external
            GIT_REPOSITORY https://github.com/mltframework/mlt.git
            GIT_TAG        v7.40.0
            GIT_SHALLOW    TRUE
            GIT_SUBMODULES ""     # skip glaxnimate (MOD_GLAXNIMATE_QT6=OFF)
            SOURCE_DIR     "${MLT_PREFIX}/src"
            BINARY_DIR     "${MLT_PREFIX}/build"
            INSTALL_DIR    "${MLT_INSTALL}"
            CMAKE_GENERATOR "${CMAKE_GENERATOR}"
            CMAKE_ARGS      ${MLT_CMAKE_ARGS}
            BUILD_BYPRODUCTS
                "${MLT_INSTALL}/lib/mlt-7.lib"
                "${MLT_INSTALL}/lib/mlt++-7.lib"
        )
    else()
        message(STATUS "MLT already installed -- skipping ExternalProject")
    endif()

    # Create IMPORTED targets pointing at the MLT install prefix.
    # ExternalProject_Add runs at build time, not configure time, so
    # these targets reference paths that will exist after the MLT build
    # step completes. The caller must add_dependencies(<target>
    # mlt_external) to enforce build order.
    #
    # CMake checks INTERFACE_INCLUDE_DIRECTORIES paths at configure time.
    # Since MLT has not been built yet, create the directories now so
    # the check passes. ExternalProject_Add overwrites them with the
    # real headers during the build step.
    file(MAKE_DIRECTORY "${MLT_INSTALL}/include/mlt-7")
    file(MAKE_DIRECTORY "${MLT_INSTALL}/include/mlt-7/mlt++")

    # MLT's framework header mlt_types.h includes <pthread.h> even though
    # MLT's CMake marks PThreads4W as PRIVATE. Find it here and add it to
    # the IMPORTED target's interface so consumers (vedit_core) get the
    # include dir transitively.
    find_package(PThreads4W REQUIRED)

    if(NOT TARGET Mlt7::mlt)
        add_library(Mlt7::mlt SHARED IMPORTED)
        set_target_properties(Mlt7::mlt PROPERTIES
            IMPORTED_LOCATION_DEBUG          "${MLT_INSTALL}/mlt-7.dll"
            IMPORTED_LOCATION_RELEASE        "${MLT_INSTALL}/mlt-7.dll"
            IMPORTED_IMPLIB_DEBUG            "${MLT_INSTALL}/lib/mlt-7.lib"
            IMPORTED_IMPLIB_RELEASE          "${MLT_INSTALL}/lib/mlt-7.lib"
            INTERFACE_INCLUDE_DIRECTORIES    "${MLT_INSTALL}/include/mlt-7"
            INTERFACE_LINK_LIBRARIES         "PThreads4W::PThreads4W"
        )
    endif()

    if(NOT TARGET Mlt7::mlt++)
        add_library(Mlt7::mlt++ SHARED IMPORTED)
        set_target_properties(Mlt7::mlt++ PROPERTIES
            IMPORTED_LOCATION_DEBUG          "${MLT_INSTALL}/mlt++-7.dll"
            IMPORTED_LOCATION_RELEASE        "${MLT_INSTALL}/mlt++-7.dll"
            IMPORTED_IMPLIB_DEBUG            "${MLT_INSTALL}/lib/mlt++-7.lib"
            IMPORTED_IMPLIB_RELEASE          "${MLT_INSTALL}/lib/mlt++-7.lib"
            INTERFACE_INCLUDE_DIRECTORIES    "${MLT_INSTALL}/include/mlt-7/mlt++"
            INTERFACE_LINK_LIBRARIES         "Mlt7::mlt"
        )
    endif()

    set(VEDIT_MLT_TARGET      "Mlt7::mlt++" PARENT_SCOPE)
    set(VEDIT_MLT_INSTALL_DIR "${MLT_INSTALL}" PARENT_SCOPE)
endfunction()
