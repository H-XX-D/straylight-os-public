add_executable(straylight-replay
    main.cpp
    replay_daemon.cpp
    recorder.cpp
    analyzer.cpp
)

target_include_directories(straylight-replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-replay PRIVATE straylight-common)
install(TARGETS straylight-replay DESTINATION bin)
