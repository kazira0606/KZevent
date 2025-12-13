add_executable(kz_unit_tests
        core_test.cpp
        log_test.cpp
        net_tests/udp_socket_test.cpp
        timer_tests/timer_test.cpp
)

target_link_libraries(kz_unit_tests
        PRIVATE
        kzevent
        GTest::gtest_main
)

target_compile_definitions(kz_unit_tests PRIVATE
        # core_test配置宏
        KZEVENT_TEST_THREAD_COUNT=16
        KZEVENT_TEST_TASKS_PER_THREAD=50
        KZEVENT_TEST_DATA_SIZE=65536
        # log_test配置宏
        KZEVENT_LOG_DIR="${CMAKE_BINARY_DIR}"
        # udp_socket_test配置宏
        KZEVENT_CLIENT_NUM=100
        # timer_test配置宏
        KZEVENT_TIMER_CLIENT_NUM=99
)

include(GoogleTest)
gtest_discover_tests(kz_unit_tests)