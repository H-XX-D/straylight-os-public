add_executable(straylight-env
    main.cpp
    env_manager.cpp
)
target_include_directories(straylight-env PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-env PRIVATE straylight-common)
install(TARGETS straylight-env DESTINATION bin)
