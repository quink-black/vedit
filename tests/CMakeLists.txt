add_executable(vedit_tests
    test_main.cpp
    test_smoke.cpp
    test_domain.cpp
    test_app_controller.cpp
    test_rpc.cpp
    test_rpc_server.cpp
)
target_link_libraries(vedit_tests PRIVATE
    vedit_rpc
    vedit_domain
    vedit_core
    vedit_util
    PkgConfig::CATCH2
)
target_compile_definitions(vedit_tests PRIVATE
    VEDIT_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
)
include(CTest)

# Register each Catch2 TEST_CASE as a separate CTest entry. This lets
# `ctest -jN` run them in parallel: each entry invokes the binary
# filtered to exactly one case. Tests have no shared state -- each
# builds its own ProjectModel/AppController and the rpc_server tests
# use unique per-PID socket paths -- so they are safe to run
# concurrently.
find_package(Catch2 REQUIRED CONFIG)
include(Catch)

# Most tests run with full ASan + LeakSanitizer coverage.
catch_discover_tests(
    vedit_tests
    TEST_SPEC "~[export]"
    TEST_SUFFIX " (sanitized)"
    PROPERTIES TIMEOUT 60
)

# The [export] tests drive a real FFmpeg avformat encoder. libx264/aac
# allocate one-shot state on internal encoder worker threads that
# FFmpeg never frees; the allocations live in unsymbolized libav code,
# so neither a name-based LSan suppression nor a per-thread
# __lsan_disable (which only covers the calling thread) can exclude
# them. The test assertions pass -- the encode produces a valid file --
# so run these with leak detection off while keeping the rest of ASan
# (use-after-free, overflow) active. Every non-export test still gets
# full leak coverage above.
catch_discover_tests(
    vedit_tests
    TEST_SPEC "[export]"
    TEST_SUFFIX " (export)"
    PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)
