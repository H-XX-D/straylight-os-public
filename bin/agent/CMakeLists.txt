add_executable(straylight-agent
    main.cpp
    agent_daemon.cpp
    task_queue.cpp
    event_loop.cpp)
target_link_libraries(straylight-agent PRIVATE straylight-common)
target_include_directories(straylight-agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-agent DESTINATION bin)
