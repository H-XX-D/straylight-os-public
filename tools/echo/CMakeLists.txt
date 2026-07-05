add_executable(straylight-echo
    main.cpp
    echo_engine.cpp
    ipc_client.cpp
)
target_include_directories(straylight-echo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-echo PRIVATE straylight-common)
install(TARGETS straylight-echo DESTINATION bin)
