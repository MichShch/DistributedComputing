add_subdirectory(common)

add_subdirectory(worker)

if(NOT DC_ONLY_WORKER AND DC_BUILD_MASTER)
    add_subdirectory(master)
endif()

if(NOT DC_ONLY_WORKER AND DC_BUILD_CLI)
    add_subdirectory(cli)
endif()
