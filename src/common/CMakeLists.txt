add_library(dc_common
    env.cpp
    time.cpp
)

target_include_directories(dc_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
