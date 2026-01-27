cmake_minimum_required(VERSION 3.20)
project(DistributedComputing LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DC_BUILD_MASTER "Build master service" ON)
option(DC_BUILD_CLI "Build CLI tool" ON)
option(DC_BUILD_TESTS "Enable tests" ON)

# Optional cross-build of Windows agent from *nix using a Windows toolchain file.
option(DC_BUILD_WIN_WORKER "Also build Windows worker via ExternalProject" OFF)
set(DC_WIN_TOOLCHAIN_FILE "" CACHE FILEPATH "CMake toolchain file for Windows cross-build")
set(DC_WIN_GENERATOR "" CACHE STRING "Generator to use for Windows cross-build (falls back to host generator if empty)")
set(DC_WIN_DEFAULT_TOOLCHAIN "${CMAKE_SOURCE_DIR}/toolchains/mingw64.cmake")

# If a toolchain file is provided and the user didn't explicitly request OFF, turn cross-build on.
if(DC_WIN_TOOLCHAIN_FILE AND NOT DC_BUILD_WIN_WORKER)
    message(STATUS "DC_WIN_TOOLCHAIN_FILE set; enabling DC_BUILD_WIN_WORKER for dual-agent build")
    set(DC_BUILD_WIN_WORKER ON CACHE BOOL "Also build Windows worker via ExternalProject" FORCE)
endif()

# Convenience: if local default toolchain exists and none specified, use it.
if(NOT DC_WIN_TOOLCHAIN_FILE AND EXISTS "${DC_WIN_DEFAULT_TOOLCHAIN}")
    set(DC_WIN_TOOLCHAIN_FILE "${DC_WIN_DEFAULT_TOOLCHAIN}" CACHE FILEPATH "CMake toolchain file for Windows cross-build" FORCE)
    if(NOT DC_BUILD_WIN_WORKER)
        message(STATUS "Using default toolchains/mingw64.cmake; enabling DC_BUILD_WIN_WORKER")
        set(DC_BUILD_WIN_WORKER ON CACHE BOOL "Also build Windows worker via ExternalProject" FORCE)
    endif()
endif()

include(CTest)
if(DC_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(DC_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Build Windows worker (cross) alongside native build when requested.
if(DC_BUILD_WIN_WORKER)
    include(ExternalProject)
    if(NOT DC_WIN_TOOLCHAIN_FILE)
        message(FATAL_ERROR "DC_BUILD_WIN_WORKER=ON requires DC_WIN_TOOLCHAIN_FILE to be set")
    endif()

    set(_dc_win_generator "${DC_WIN_GENERATOR}")
    if(_dc_win_generator STREQUAL "")
        set(_dc_win_generator "${CMAKE_GENERATOR}")
    endif()

    ExternalProject_Add(dc_worker_win
        SOURCE_DIR ${CMAKE_SOURCE_DIR}
        BINARY_DIR ${CMAKE_BINARY_DIR}/win-worker
        CMAKE_GENERATOR ${_dc_win_generator}
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${DC_WIN_TOOLCHAIN_FILE}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DDC_BUILD_MASTER=OFF
            -DDC_BUILD_CLI=OFF
            -DDC_BUILD_TESTS=OFF
            -DDC_ONLY_WORKER=ON
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target dc_worker
        INSTALL_COMMAND ""
    )

    add_custom_target(build_win_worker ALL DEPENDS dc_worker_win)
endif()
