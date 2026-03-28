cmake_minimum_required(VERSION 4.0)

project(
    dc_common
    VERSION 1.0
    LANGUAGES CXX
)

file(GLOB_RECURSE PROJECT_SRC CONFIGURE_DEPENDS "*.cpp")
set(PROJECT_INC "")
set(PROJECT_LIB "")
set(PROJECT_ADD_LIB "")

list(APPEND PROJECT_INC ${CMAKE_CURRENT_SOURCE_DIR})
set(PROJECT_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

list(APPEND PROJECT_LIB spdlog::spdlog)

if(WIN32)
    list(APPEND PROJECT_LIB ws2_32)
endif()

include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG origin/v1.x
    GIT_SHALLOW true
)
FetchContent_MakeAvailable(spdlog)

add_library(${PROJECT_NAME} ${PROJECT_SRC})

target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_INC})

target_link_libraries(${PROJECT_NAME} PUBLIC ${PROJECT_LIB})
