cmake_minimum_required(VERSION 3.10)

project(tiny-shell
    VERSION 0.1.0
    DESCRIPTION "A lightweight shell for embedded systems"
    LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
endif()
add_subdirectory(example)