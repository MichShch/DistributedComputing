add_executable(dc_worker
    agent_client.cpp
    task_executor.cpp
    worker_app.cpp
    main.cpp
)

target_include_directories(dc_worker
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(dc_worker
    PRIVATE
        dc_common
)

if(WIN32)
    target_link_libraries(dc_worker PRIVATE ws2_32)
    # Make Windows agent self-contained: link libstdc++, libgcc, and winpthread statically.
    target_link_options(dc_worker PRIVATE
        "-static-libstdc++"
        "-static-libgcc"
        "-Wl,-Bstatic"
        "-lwinpthread"
        "-Wl,-Bdynamic"
    )
endif()
