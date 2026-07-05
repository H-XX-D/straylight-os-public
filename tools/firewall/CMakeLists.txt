add_executable(straylight-firewall
    main.cpp
    firewall_manager.cpp
)
target_include_directories(straylight-firewall PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-firewall PRIVATE straylight-common)
install(TARGETS straylight-firewall DESTINATION bin)
