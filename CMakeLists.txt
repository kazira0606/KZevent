cmake_minimum_required(VERSION 3.20)

project(KZevent
    VERSION 0.1.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

set(KZEVENT_SOURCES
        src/core.cpp
        src/log.cpp
        src/net/udp_socket.cpp
        include/core.hpp
        include/sys_error.hpp
        include/log.hpp
        include/net/udp_socket.hpp
)

add_library(kzevent ${KZEVENT_SOURCES})

target_link_libraries(kzevent PRIVATE Threads::Threads)

target_include_directories(kzevent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(kzevent PRIVATE KZ_EVENT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(kzevent PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(KZEVENT_BUILD_EXAMPLES "Build KZevent examples" ON)

# if (KZEVENT_BUILD_EXAMPLES)
#     add_subdirectory(examples)
# endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
