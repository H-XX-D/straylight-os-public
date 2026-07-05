add_executable(straylight-link
    main.cpp
    link_manager.cpp
)
target_include_directories(straylight-link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-link PRIVATE straylight-common)
install(TARGETS straylight-link DESTINATION bin)
