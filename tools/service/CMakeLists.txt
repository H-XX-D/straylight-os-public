add_executable(straylight-service
    main.cpp
    service_manager.cpp
)
target_include_directories(straylight-service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-service PRIVATE straylight-common)
install(TARGETS straylight-service DESTINATION bin)
