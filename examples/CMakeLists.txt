set(KZEVENT_EXAMPLES
    tcp_example
    timer_example
    udp_example
    unix_example
)

foreach(example_name ${KZEVENT_EXAMPLES})
    add_executable(${example_name} ${example_name}.cpp)
    target_link_libraries(${example_name} PRIVATE kzevent)
endforeach()
