cmake_minimum_required(VERSION 3.14)

project(KZevent
        VERSION 0.1.0
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(ENABLE_ASAN "Enable AddressSanitizer + UBSan + LeakSanitizer" OFF)
option(KZEVENT_BUILD_TESTS "Build tests" OFF)
option(KZEVENT_BUILD_EXAMPLES "Build example programs" OFF)

if (ENABLE_ASAN)
    set(SAN_FLAGS -fsanitize=address,leak,undefined -fno-omit-frame-pointer)
    add_compile_options(${SAN_FLAGS} -g)
    add_link_options(${SAN_FLAGS})
endif()

find_package(Threads REQUIRED)

if(KZEVENT_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_subdirectory(src)

if(KZEVENT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(KZEVENT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()