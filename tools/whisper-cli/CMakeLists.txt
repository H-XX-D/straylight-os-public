add_executable(straylight-whisper-cli
    main.cpp
)
target_include_directories(straylight-whisper-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-whisper-cli PRIVATE straylight-common)
install(TARGETS straylight-whisper-cli DESTINATION bin)
