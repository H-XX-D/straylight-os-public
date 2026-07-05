add_executable(straylight-input
    main.cpp
    input_manager.cpp
)
target_include_directories(straylight-input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-input PRIVATE straylight-common)
install(TARGETS straylight-input DESTINATION bin)
