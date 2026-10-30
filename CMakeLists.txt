cmake_minimum_required(VERSION 3.20)
project(ray_tracer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# Header-only engine core. Everything that links it gets the include path and threads.
add_library(engine INTERFACE)
target_include_directories(engine INTERFACE engine)
target_link_libraries(engine INTERFACE Threads::Threads)

# Vendored single-header libraries, included as system headers so their warnings stay quiet.
add_library(third_party INTERFACE)
target_include_directories(third_party SYSTEM INTERFACE third_party)

function(set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4100)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endfunction()

# Command-line renderer
add_executable(rt-cli apps/cli/main.cc apps/cli/image_io.cc)
target_link_libraries(rt-cli PRIVATE engine third_party)
set_project_warnings(rt-cli)
if(MINGW)
    # Link the runtime statically so the .exe runs outside the MSYS2 shell.
    target_link_options(rt-cli PRIVATE -static)
endif()

# Unit tests
include(CTest)
if(BUILD_TESTING)
    add_executable(tests
        tests/test_main.cc
        tests/test_math.cc
        tests/test_geometry.cc
        tests/test_render.cc)
    target_link_libraries(tests PRIVATE engine third_party)
    set_project_warnings(tests)
    if(MINGW)
        target_link_options(tests PRIVATE -static)
    endif()
    add_test(NAME unit_tests COMMAND tests)
endif()
