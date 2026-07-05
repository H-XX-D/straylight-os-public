add_executable(straylight-voice-cli
    main.cpp
)
target_include_directories(straylight-voice-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-voice-cli PRIVATE straylight-common)
install(TARGETS straylight-voice-cli DESTINATION bin)
