set(PROJECT_CFLAGS "")
set(PROJECT_CXXFLAGS "")

if(WIN32)
    list(APPEND PROJECT_CFLAGS "${CMAKE_C_FLAGS} -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00")
    list(APPEND PROJECT_CXXFLAGS "${CMAKE_C_FLAGS} -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00")
endif()

list(APPEND CMAKE_C_FLAGS   "${PROJECT_CFLAGS}")
list(APPEND CMAKE_CXX_FLAGS "${PROJECT_CXXFLAGS}")

add_subdirectory(common)

add_subdirectory(worker)

if(NOT DC_ONLY_WORKER AND DC_BUILD_MASTER)
    add_subdirectory(master)
endif()

if(NOT DC_ONLY_WORKER AND DC_BUILD_CLI)
    add_subdirectory(cli)
endif()
