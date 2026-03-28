cmake_minimum_required(VERSION 4.0)

project(
    dc_worker
    VERSION 1.0
    LANGUAGES CXX
)

file(GLOB_RECURSE PROJECT_SRC CONFIGURE_DEPENDS "*.cpp")
set(PROJECT_INC "dc_common")
set(PROJECT_LIB "dc_common")

list(APPEND PROJECT_INC ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND PROJECT_INC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(${PROJECT_NAME} ${PROJECT_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INC})

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_LIB})

if(WIN32)
    # Produce a standalone MinGW executable without shipping runtime DLLs.
    target_link_options(${PROJECT_NAME} PRIVATE
        "-static"
        "-static-libstdc++"
        "-static-libgcc"
        "-Wl,-Bstatic"
        "-lwinpthread"
    )
endif()
