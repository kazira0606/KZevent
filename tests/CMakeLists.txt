add_executable(unit_tests
        core_test.cpp
        log_test.cpp
)

target_link_libraries(unit_tests
        PRIVATE
        kzevent
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)