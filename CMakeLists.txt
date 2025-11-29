cmake_minimum_required(VERSION 3.20)

project(KZevent
        VERSION 0.1.0
        LANGUAGES CXX)

option(ENABLE_ASAN "Enable AddressSanitizer + UBSan + LeakSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if (ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled at the same time!")
endif()

if (ENABLE_ASAN)
    set(SAN_FLAGS -fsanitize=address,leak,undefined -fno-omit-frame-pointer)
    add_compile_options(${SAN_FLAGS} -g)
    add_link_options(${SAN_FLAGS})
endif()

if (ENABLE_TSAN)
    set(SAN_FLAGS -fsanitize=thread)
    add_compile_options(${SAN_FLAGS} -g)
    add_link_options(${SAN_FLAGS})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

