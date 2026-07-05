add_executable(straylight-color
    main.cpp
    color_engine.cpp
)
target_include_directories(straylight-color PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-color PRIVATE straylight-common)
install(TARGETS straylight-color DESTINATION bin)
