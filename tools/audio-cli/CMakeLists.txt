add_executable(straylight-audio-cli
    main.cpp
)
target_include_directories(straylight-audio-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-audio-cli PRIVATE straylight-common)
install(TARGETS straylight-audio-cli DESTINATION bin)
