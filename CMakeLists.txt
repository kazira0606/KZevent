cmake_minimum_required(VERSION 3.20)

project(KZevent
        VERSION 0.1.0
        LANGUAGES CXX)

option(ENABLE_ASAN "Enable AddressSanitizer + UBSan + LeakSanitizer" OFF)

if (ENABLE_ASAN)
    set(SAN_FLAGS -fsanitize=address,leak,undefined -fno-omit-frame-pointer)
    add_compile_options(${SAN_FLAGS} -g)
    add_link_options(${SAN_FLAGS})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

