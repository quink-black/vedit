add_executable(vedit_tests
    test_smoke.cpp
    test_domain.cpp
    test_app_controller.cpp
)
target_link_libraries(vedit_tests PRIVATE
    vedit_domain
    vedit_core
    vedit_util
    PkgConfig::CATCH2
)
target_compile_definitions(vedit_tests PRIVATE
    VEDIT_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
)
include(CTest)
add_test(NAME vedit_tests COMMAND vedit_tests)
