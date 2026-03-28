cmake_minimum_required(VERSION 4.0)

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

add_executable(${PROJECT_NAME} ${PROJECT_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INC})

find_package(Threads REQUIRED)

find_package(PQXX QUIET)
if(NOT PQXX_FOUND)
    find_package(libpqxx QUIET)
endif()

if(PQXX_FOUND)
    if(TARGET pqxx::pqxx)
        set(PQXX_TARGET pqxx::pqxx)
    elseif(TARGET PQXX::PQXX)
        set(PQXX_TARGET PQXX::PQXX)
    else()
        set(PQXX_TARGET pqxx)
    endif()
elseif(libpqxx_FOUND)
    set(PQXX_TARGET pqxx)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBPQXX QUIET libpqxx)
    endif()
    if(LIBPQXX_FOUND)
        target_include_directories(dc_master PRIVATE ${LIBPQXX_INCLUDE_DIRS})
        set(PQXX_TARGET ${LIBPQXX_LIBRARIES})
    endif()
endif()

if(NOT PQXX_TARGET)
    message(FATAL_ERROR "libpqxx not found. Install libpqxx and libpq-dev (Linux) or build/install libpqxx (Windows).")
endif()

list(APPEND PROJECT_LIB Threads::Threads)
list(APPEND PROJECT_LIB ${PQXX_TARGET})

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_LIB})
