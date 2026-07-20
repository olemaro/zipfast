cmake_minimum_required(VERSION 3.16)
project(zipfast CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /W3)
else()
    add_compile_options(-O2 -Wall)
endif()

add_executable(zipfast zipfast.cpp miniz.c)
target_include_directories(zipfast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
