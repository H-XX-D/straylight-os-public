add_executable(straylight-http
    main.cpp
    http_client.cpp
)
target_include_directories(straylight-http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-http PRIVATE straylight-common)
install(TARGETS straylight-http DESTINATION bin)
