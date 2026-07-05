add_executable(straylight-swarm
    main.cpp
    swarm_daemon.cpp
    discovery.cpp
    orchestrator.cpp
    node_client.cpp
)
target_link_libraries(straylight-swarm PRIVATE straylight-common)
target_include_directories(straylight-swarm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-swarm DESTINATION bin)
