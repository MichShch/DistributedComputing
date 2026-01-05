include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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
        set(PQXX_TARGET ${LIBPQXX_LIBRARIES})
        set(PQXX_INCLUDE_DIRS ${LIBPQXX_INCLUDE_DIRS})
    endif()
endif()

if(NOT PQXX_TARGET)
    message(FATAL_ERROR "libpqxx not found. Install libpqxx and libpq-dev (Linux) or build/install libpqxx (Windows).")
endif()

add_executable(master_integration_test
    master_integration_test.cpp
)

target_include_directories(master_integration_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party
        ${PQXX_INCLUDE_DIRS}
)

target_link_libraries(master_integration_test
    PRIVATE
        GTest::gtest
        ${PQXX_TARGET}
)

add_dependencies(master_integration_test dc_master)

add_test(NAME master_integration_test COMMAND master_integration_test)
set_tests_properties(master_integration_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT
        "DC_MASTER_BIN=$<TARGET_FILE:dc_master>;DC_TEST_ENV_FILE=${PROJECT_SOURCE_DIR}/configs/master.env"
)
