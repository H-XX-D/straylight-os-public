add_executable(straylight-fonts
    main.cpp
    font_manager.cpp
)
target_include_directories(straylight-fonts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-fonts PRIVATE straylight-common)
install(TARGETS straylight-fonts DESTINATION bin)
