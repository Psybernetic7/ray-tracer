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

# Browser build. Configured through Emscripten (emcmake), this builds only the WebAssembly
# module for apps/web, in one of two flavours: multithreaded, which needs a cross-origin
# isolated page for SharedArrayBuffer, and single-threaded, the fallback for every other page.
if(EMSCRIPTEN)
    option(RT_WEB_THREADS "Build the multithreaded WebAssembly module" ON)
    set(RT_WEB_OUT "${CMAKE_SOURCE_DIR}/apps/web/public/wasm" CACHE PATH "Where the module is written")

    add_executable(studio apps/web/native/bindings.cc)
    target_include_directories(studio PRIVATE apps/web/native)
    target_link_libraries(studio PRIVATE engine third_party)
    set_project_warnings(studio)

    set(web_flags -O3 -msimd128)
    set(web_link_flags
        -lembind
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createStudio
        -sENVIRONMENT=web,worker
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=4GB
        -sSTACK_SIZE=2MB)
    if(RT_WEB_THREADS)
        list(APPEND web_flags -pthread)
        # One worker per core is started with the module, so threads are ready before the
        # first render: a thread that had to be started mid-render would deadlock it.
        list(APPEND web_link_flags -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency
                                   -sDEFAULT_PTHREAD_STACK_SIZE=1MB
                                   # Growable shared memory makes JavaScript's view of it a little
                                   # slower; nearly all the time is spent in WebAssembly anyway.
                                   -Wno-pthreads-mem-growth)
        set(web_name studio-mt)
    else()
        set(web_name studio-st)
    endif()

    target_compile_options(studio PRIVATE ${web_flags})
    target_link_options(studio PRIVATE ${web_flags} ${web_link_flags})
    set_target_properties(studio PROPERTIES
        OUTPUT_NAME ${web_name}
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY ${RT_WEB_OUT})
    return()
endif()

# Command-line renderer
add_executable(rt-cli apps/cli/main.cc apps/cli/analyze.cc apps/cli/dem_source.cc apps/cli/image_io.cc)
target_link_libraries(rt-cli PRIVATE engine third_party)
# Bundled elevation data for --place, found from any working directory.
target_compile_definitions(rt-cli PRIVATE RT_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
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
        tests/test_render.cc
        tests/test_terrain.cc
        tests/test_analysis.cc
        tests/test_dem.cc
        tests/test_studio.cc)
    target_link_libraries(tests PRIVATE engine third_party)
    target_include_directories(tests PRIVATE apps/web/native)
    set_project_warnings(tests)
    if(MINGW)
        target_link_options(tests PRIVATE -static)
    endif()
    add_test(NAME unit_tests COMMAND tests)

    # End to end, offline: a bundled place through PNG decoding, meshing, BVH and a tiny render,
    # then a tiny analysis on the same data.
    add_test(NAME place_render
             COMMAND rt-cli --place fuji --res 64 --width 48 --spp 1 --quiet
                     --out ${CMAKE_BINARY_DIR}/smoke-place.png)
    add_test(NAME place_analyze
             COMMAND rt-cli analyze --place matterhorn --res 64 --grid 16 --day-step 30
                     --no-render --out ${CMAKE_BINARY_DIR}/smoke-analyze.png)
endif()
