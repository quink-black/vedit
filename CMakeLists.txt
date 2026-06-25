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
    add_compile_options(-fsanitize=address -g)
    add_link_options(-fsanitize=address)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(Threads REQUIRED)

# Asio (standalone, header-only) -- transport for the in-process JSON-RPC
# server (vedit_rpc). Resolved at top level so the `asio::asio` target is
# visible from every subdirectory. Detection mirrors idiff: find_path first,
# fall back to FetchContent if absent.
find_path(ASIO_INCLUDE_DIR asio.hpp
    HINTS /usr/local/include /usr/include
          $ENV{HOMEBREW_PREFIX}/include
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

# ---- macOS / Apple platform detection ----
if(APPLE)
    message(STATUS "Detected macOS / Apple platform")
    target_compile_definitions(vedit_util PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_core PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_domain PUBLIC VEDIT_PLATFORM_MACOS)
    target_compile_definitions(vedit_rpc PUBLIC VEDIT_PLATFORM_MACOS)

    # Apple Clang may need -fno-aligned-allocation until C++17 ABI is stable
    add_compile_options($<$<CXX_COMPILER_ID:AppleClang>:-fno-aligned-allocation>)

    # Framework search paths for MLT dependencies on macOS
    # Homebrew installs frameworks/libraries under its prefix
    if(DEFINED ENV{HOMEBREW_PREFIX})
        set(HOMEBREW_PREFIX "$ENV{HOMEBREW_PREFIX}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(HOMEBREW_PREFIX "/opt/homebrew")
    else()
        set(HOMEBREW_PREFIX "/usr/local")
    endif()
    list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
    list(APPEND CMAKE_FRAMEWORK_PATH "${HOMEBREW_PREFIX}/Frameworks")
    list(APPEND CMAKE_LIBRARY_PATH "${HOMEBREW_PREFIX}/lib")
    list(APPEND CMAKE_INCLUDE_PATH "${HOMEBREW_PREFIX}/include")
    message(STATUS "Using Homebrew prefix: ${HOMEBREW_PREFIX}")
endif()

if(VEDIT_BUILD_TESTS)
    pkg_check_modules(CATCH2 REQUIRED IMPORTED_TARGET catch2-with-main)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src/util)
add_subdirectory(src/core)
add_subdirectory(src/domain)
add_subdirectory(src/rpc)
add_subdirectory(src/app)

if(VEDIT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
