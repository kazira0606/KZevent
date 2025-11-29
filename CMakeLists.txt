cmake_minimum_required(VERSION 3.20)

project(KZevent
        VERSION 0.1.0
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)