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
        dc_common
)

add_dependencies(master_integration_test dc_master)

add_test(NAME master_integration_test COMMAND master_integration_test)
set_tests_properties(master_integration_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT
        "DC_MASTER_BIN=$<TARGET_FILE:dc_master>;DC_TEST_ENV_FILE=${PROJECT_SOURCE_DIR}/configs/master.env"
)

add_executable(worker_integration_test
    worker_integration_test.cpp
)

target_include_directories(worker_integration_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party
        ${PQXX_INCLUDE_DIRS}
)

target_link_libraries(worker_integration_test
    PRIVATE
        GTest::gtest
        ${PQXX_TARGET}
        dc_common
)

add_dependencies(worker_integration_test dc_master dc_worker)

add_test(NAME worker_integration_test COMMAND worker_integration_test)
set_tests_properties(worker_integration_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT
        "DC_MASTER_BIN=$<TARGET_FILE:dc_master>;DC_WORKER_BIN=$<TARGET_FILE:dc_worker>;DC_TEST_ENV_FILE=${PROJECT_SOURCE_DIR}/configs/master.env"
)

add_executable(common_unit_tests
    common_tests.cpp
)

target_include_directories(common_unit_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(common_unit_tests
    PRIVATE
        GTest::gtest_main
        dc_common
)

add_test(NAME common_unit_tests COMMAND common_unit_tests)

add_executable(master_unit_tests
    master_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/master/api_mappers.cpp
    ${PROJECT_SOURCE_DIR}/src/master/log_store.cpp
)

target_include_directories(master_unit_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(master_unit_tests
    PRIVATE
        GTest::gtest_main
        dc_common
)

add_test(NAME master_unit_tests COMMAND master_unit_tests)

add_executable(cli_integration_test
    cli_integration_test.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/api_client.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/formatters.cpp
)

target_include_directories(cli_integration_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
        ${PQXX_INCLUDE_DIRS}
)

target_compile_definitions(cli_integration_test
    PRIVATE
        DC_CLI_DISABLE_MAIN
)

target_link_libraries(cli_integration_test
    PRIVATE
        GTest::gtest_main
        dc_common
        ${PQXX_TARGET}
)

add_dependencies(cli_integration_test dc_master)

add_test(NAME cli_integration_test COMMAND cli_integration_test)
set_tests_properties(cli_integration_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT
        "DC_MASTER_BIN=$<TARGET_FILE:dc_master>;DC_TEST_ENV_FILE=${PROJECT_SOURCE_DIR}/configs/master.env"
)

add_executable(cli_unit_tests
    cli_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/formatters.cpp
)

target_include_directories(cli_unit_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
)

target_compile_definitions(cli_unit_tests
    PRIVATE
        DC_CLI_DISABLE_MAIN
)

target_link_libraries(cli_unit_tests
    PRIVATE
        GTest::gtest_main
        dc_common
)

add_test(NAME cli_unit_tests COMMAND cli_unit_tests)

add_executable(worker_unit_tests
    worker_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/worker/agent_client.cpp
    ${PROJECT_SOURCE_DIR}/src/worker/task_executor.cpp
    ${PROJECT_SOURCE_DIR}/src/worker/worker_app.cpp
)

target_include_directories(worker_unit_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(worker_unit_tests
    PRIVATE
        GTest::gmock_main
        dc_common
)

add_test(NAME worker_unit_tests COMMAND worker_unit_tests)
