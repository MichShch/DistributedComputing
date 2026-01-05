add_executable(dc_master
    control_service.cpp
    main.cpp
    storage.cpp
    log_store.cpp
)

target_include_directories(dc_master
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
)

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

target_link_libraries(dc_master
    PRIVATE
        dc_common
        ${PQXX_TARGET}
)
