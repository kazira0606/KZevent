add_executable(unit_tests
        core_test.cpp
        log_test.cpp
)

target_link_libraries(unit_tests
        PRIVATE
        kzevent
        GTest::gtest_main
)

target_compile_definitions(unit_tests PRIVATE KZ_EVENT_LOG_DIR=\"${CMAKE_BINARY_DIR}\")

include(GoogleTest)
gtest_discover_tests(unit_tests)