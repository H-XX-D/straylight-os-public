add_executable(straylight-whisper
    main.cpp
    channel.cpp
    crypto_channel.cpp
)
target_include_directories(straylight-whisper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-whisper PRIVATE straylight-common pthread)
install(TARGETS straylight-whisper DESTINATION bin)
