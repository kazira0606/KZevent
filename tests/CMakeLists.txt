add_executable(unit_tests
        core_test.cpp
        log_test.cpp
)

target_link_libraries(unit_tests
        PRIVATE
        kzevent
        GTest::gtest_main
)

# core_test用
target_compile_definitions(unit_tests PRIVATE KZEVENT_TEST_THREAD_COUNT=16)
target_compile_definitions(unit_tests PRIVATE KZEVENT_TEST_TASKS_PER_THREAD=50)
target_compile_definitions(unit_tests PRIVATE KZEVENT_TEST_DATA_SIZE=(64 * 1024))
# log_test用
target_compile_definitions(unit_tests PRIVATE KZ_EVENT_LOG_DIR=\"${CMAKE_BINARY_DIR}\")

include(GoogleTest)
gtest_discover_tests(unit_tests)