cmake_minimum_required(VERSION 3.20)
project(vedit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VEDIT_BUILD_TESTS "Build unit tests" ON)
option(VEDIT_ENABLE_ASAN "Enable AddressSanitizer (opt-in, any build type)" OFF)

if(VEDIT_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---- macOS / Apple platform detection ----
if(APPLE)
    message(STATUS "Detected macOS / Apple platform")

    # Apple Clang may need -fno-aligned-allocation until C++17 ABI is stable
    add_compile_options($<$<CXX_COMPILER_ID:AppleClang>:-fno-aligned-allocation>)

    if(NOT CMAKE_PREFIX_PATH)
        message(WARNING
            "CMAKE_PREFIX_PATH is empty on macOS. Set it to your package "
            "prefix (e.g. -DCMAKE_PREFIX_PATH=/opt/homebrew) so CMake can "
            "find Homebrew dependencies.")
    endif()
endif()

find_package(nlohmann_json REQUIRED)

# ---- Dependency resolution ----
#
# Two paths:
#   * WIN32 + MSVC:  vcpkg manifest mode provides all deps except MLT.
#                    MLT is built from source via ExternalProject_Add
#                    (see cmake/BuildMlt.cmake) because it is not in the
#                    vcpkg registry and its upstream vcpkg.json pulls
#                    Qt/opencv/gdk-pixbuf, which vedit does not use.
#   * Other (Linux, macOS): PkgConfig against system packages, as before.
#
# VEDIT_*_TARGET variables hold the consumer-facing target name on each
# branch so the subdirectory CMakeLists.txt files stay platform-agnostic.
if(WIN32 AND MSVC)
    message(STATUS "Detected Windows / MSVC -- using vcpkg dependencies")

    find_package(SDL2 CONFIG REQUIRED)
    find_package(asio CONFIG REQUIRED)
    if(VEDIT_BUILD_TESTS)
        find_package(Catch2 CONFIG REQUIRED)
    endif()

    set(VEDIT_SDL2_TARGET   SDL2::SDL2)
    set(VEDIT_ASIO_TARGET   asio::asio)
    set(VEDIT_CATCH2_TARGET Catch2::Catch2WithMain)

    # _WIN32_WINNT=0x0A00 targets Windows 10 (1607+), the floor for
    # both SetThreadDescription and AF_UNIX (Win10 1803+). NOMINMAX
    # keeps std::min/std::max usable. ASIO_HAS_LOCAL_SOCKETS enables
    # asio::local::stream_protocol on Windows.
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX
                            ASIO_STANDALONE ASIO_HAS_LOCAL_SOCKETS)

    # VEDIT_MLT_TARGET is set by the MLT ExternalProject block included
    # below. It is intentionally empty here so this branch stays valid
    # even before BuildMlt.cmake is wired in.
    set(VEDIT_MLT_TARGET "")

    # NFD is always built from source via FetchContent on Windows.
    set(VEDIT_BUILD_NFD ON)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(VEDIT_MLT_TARGET PkgConfig::MLT)
    set(VEDIT_SDL2_TARGET PkgConfig::SDL2)

    find_package(nfd QUIET)
    if(NOT nfd_FOUND)
        message(STATUS "nfd not found on system -- will build from source")
        set(VEDIT_BUILD_NFD ON)
    else()
        message(STATUS "Found system nfd: ${nfd_DIR}")
        set(VEDIT_BUILD_NFD OFF)
    endif()

    # Asio (standalone, header-only) -- transport for the in-process
    # JSON-RPC server (vedit_rpc). Resolved at top level so the
    # `asio::asio` target is visible from every subdirectory. Detection
    # mirrors idiff: find_path first, fall back to FetchContent if absent.
    find_path(ASIO_INCLUDE_DIR asio.hpp
        DOC "Path to standalone Asio headers (asio.hpp)")
    if(ASIO_INCLUDE_DIR)
        message(STATUS "Found system Asio: ${ASIO_INCLUDE_DIR}")
        add_library(asio_asio INTERFACE)
        target_include_directories(asio_asio INTERFACE "${ASIO_INCLUDE_DIR}")
        target_compile_definitions(asio_asio INTERFACE ASIO_STANDALONE)
        add_library(asio::asio ALIAS asio_asio)
    else()
        message(FATAL_ERROR "Asio not found. Install libasio-dev (apt) or equivalent.")
    endif()
    set(VEDIT_ASIO_TARGET asio::asio)

    if(VEDIT_BUILD_TESTS)
        pkg_check_modules(CATCH2 REQUIRED IMPORTED_TARGET catch2-with-main)
        set(VEDIT_CATCH2_TARGET PkgConfig::CATCH2)
    endif()
endif()

find_package(Threads REQUIRED)

if(VEDIT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src/util)
add_subdirectory(src/core)
add_subdirectory(src/domain)
add_subdirectory(src/rpc)
add_subdirectory(src/app)

# ---- macOS platform defines (must be after add_subdirectory so targets exist) ----
if(APPLE)
    target_compile_definitions(vedit_util PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_core PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_domain PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_rpc PUBLIC VEDIT_PLATFORM_MACOS)
endif()

if(VEDIT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
