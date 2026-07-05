add_executable(straylight-cgroup
    main.cpp
    cgroup_manager.cpp
)
target_include_directories(straylight-cgroup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-cgroup PRIVATE straylight-common)
install(TARGETS straylight-cgroup DESTINATION bin)
