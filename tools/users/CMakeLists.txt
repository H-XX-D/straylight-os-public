add_executable(straylight-users
    main.cpp
    user_manager.cpp
)
target_include_directories(straylight-users PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-users PRIVATE straylight-common)
install(TARGETS straylight-users DESTINATION bin)
