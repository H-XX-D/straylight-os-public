add_executable(straylight-audio
    main.cpp
    audio_engine.cpp
)
target_include_directories(straylight-audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-audio PRIVATE straylight-common)
install(TARGETS straylight-audio DESTINATION bin)
