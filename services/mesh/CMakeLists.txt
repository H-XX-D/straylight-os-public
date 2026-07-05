add_executable(straylight-mesh
    main.cpp
    mesh_daemon.cpp
    gpu_pool.cpp
    mesh_monitor.cpp
)
target_link_libraries(straylight-mesh PRIVATE straylight-common)
target_include_directories(straylight-mesh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-mesh DESTINATION bin)
