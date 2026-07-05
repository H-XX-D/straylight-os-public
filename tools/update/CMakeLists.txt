add_executable(straylight-update
    main.cpp
    update_manager.cpp
)
target_include_directories(straylight-update PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-update PRIVATE straylight-common)
install(TARGETS straylight-update DESTINATION bin)
