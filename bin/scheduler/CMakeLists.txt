add_executable(straylight-scheduler main.cpp scheduler_daemon.cpp topology.cpp cgroup.cpp)
target_link_libraries(straylight-scheduler PRIVATE straylight-common)
target_include_directories(straylight-scheduler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-scheduler DESTINATION bin)
