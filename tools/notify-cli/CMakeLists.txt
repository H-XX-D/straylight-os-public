add_executable(straylight-notify-cli
    main.cpp
)
target_include_directories(straylight-notify-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-notify-cli PRIVATE straylight-common)
install(TARGETS straylight-notify-cli DESTINATION bin)
