set(KZEVENT_SOURCES
        core.cpp
        log.cpp
        stream_buffer.cpp
        net/inet.cpp
        net/tcp_socket.cpp
        net/udp_socket.cpp
        net/unix_socket.cpp
        timer/timer.cpp
)

add_library(kzevent ${KZEVENT_SOURCES})

file(GLOB_RECURSE KZEVENT_HEADERS CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/include/kzevent/*.hpp
)

target_sources(kzevent PRIVATE ${KZEVENT_HEADERS})

target_link_libraries(kzevent PUBLIC Threads::Threads)

target_include_directories(kzevent PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_definitions(kzevent PRIVATE KZ_EVENT_LOG_DIR="${CMAKE_BINARY_DIR}")

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(kzevent PRIVATE -Wall -Wextra -Wpedantic)
endif()