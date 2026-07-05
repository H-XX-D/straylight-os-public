add_executable(straylight-clip
    main.cpp
    clip_manager.cpp
)
target_include_directories(straylight-clip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-clip PRIVATE straylight-common)
install(TARGETS straylight-clip DESTINATION bin)
