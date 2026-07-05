add_executable(straylight-sysctl
    main.cpp
    sysctl_manager.cpp
)
target_include_directories(straylight-sysctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-sysctl PRIVATE straylight-common)
install(TARGETS straylight-sysctl DESTINATION bin)
