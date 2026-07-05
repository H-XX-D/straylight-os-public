add_executable(straylight-cert
    main.cpp
    cert_manager.cpp
)
target_include_directories(straylight-cert PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-cert PRIVATE straylight-common)
install(TARGETS straylight-cert DESTINATION bin)
