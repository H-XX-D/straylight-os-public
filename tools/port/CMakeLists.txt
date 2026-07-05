add_executable(straylight-port
    main.cpp
    port_manager.cpp
)
target_include_directories(straylight-port PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-port PRIVATE straylight-common)
install(TARGETS straylight-port DESTINATION bin)
