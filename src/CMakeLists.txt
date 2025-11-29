set(KZEVENT_SOURCES
        core.cpp
        log.cpp
        net/udp_socket.cpp
        ${CMAKE_SOURCE_DIR}/include/kzevent/core.hpp
        ${CMAKE_SOURCE_DIR}/include/kzevent/log.hpp
        ${CMAKE_SOURCE_DIR}/include/kzevent/sys_error.hpp
        ${CMAKE_SOURCE_DIR}/include/kzevent/net/udp_socket.hpp
)

add_library(kzevent ${KZEVENT_SOURCES})

target_link_libraries(kzevent PRIVATE Threads::Threads)

target_include_directories(kzevent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(kzevent PRIVATE KZ_EVENT_LOG_DIR=\"${CMAKE_BINARY_DIR}\")

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(kzevent PRIVATE -Wall -Wextra -Wpedantic)
endif()