add_executable(straylight-theme
    main.cpp
    theme_engine.cpp
)
target_include_directories(straylight-theme PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-theme PRIVATE straylight-common)
install(TARGETS straylight-theme DESTINATION bin)
