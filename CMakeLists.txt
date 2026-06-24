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
option(VEDIT_ENABLE_SANITIZERS "Enable ASan+UBSan (Debug only)" ON)

if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address -fsanitize=undefined -g)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(Threads REQUIRED)

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
