add_executable(straylight-git
    main.cpp
    git_helper.cpp
)
target_include_directories(straylight-git PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-git PRIVATE straylight-common)
install(TARGETS straylight-git DESTINATION bin)
