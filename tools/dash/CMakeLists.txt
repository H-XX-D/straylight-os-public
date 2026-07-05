add_executable(straylight-dash
    main.cpp
    tui.cpp
    widgets.cpp
)
target_include_directories(straylight-dash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-dash PRIVATE straylight-common pthread)
install(TARGETS straylight-dash DESTINATION bin)
