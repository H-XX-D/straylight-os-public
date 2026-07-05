add_executable(straylight-display
    main.cpp
    display_manager.cpp
)
target_include_directories(straylight-display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-display PRIVATE straylight-common)
install(TARGETS straylight-display DESTINATION bin)
