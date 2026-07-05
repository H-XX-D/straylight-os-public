add_executable(straylight-watchdog-cli
    main.cpp
)
target_include_directories(straylight-watchdog-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-watchdog-cli PRIVATE straylight-common)
install(TARGETS straylight-watchdog-cli DESTINATION bin)
