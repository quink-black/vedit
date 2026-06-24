add_executable(vedit_tests
    test_smoke.cpp
)
target_link_libraries(vedit_tests PRIVATE
    vedit_domain
    vedit_core
    vedit_util
    PkgConfig::CATCH2
)
include(CTest)
add_test(NAME vedit_tests COMMAND vedit_tests)
