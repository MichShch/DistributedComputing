cmake_minimum_required(VERSION 3.20)

project(
    dc_master
    VERSION 1.0
    LANGUAGES CXX
)

file(GLOB_RECURSE PROJECT_SRC CONFIGURE_DEPENDS "*.cpp")
set(PROJECT_INC "dc_common")
set(PROJECT_LIB "dc_common")

list(APPEND PROJECT_INC ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND PROJECT_INC ${CMAKE_CURRENT_SOURCE_DIR}/..)
list(APPEND PROJECT_INC "dc_broker")

add_executable(${PROJECT_NAME} ${PROJECT_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INC})

find_package(Threads REQUIRED)
list(APPEND PROJECT_LIB Threads::Threads)
list(APPEND PROJECT_LIB "dc_broker")

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_LIB})
