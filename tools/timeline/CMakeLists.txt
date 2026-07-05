add_executable(straylight-timeline
    main.cpp
    event_store.cpp
    collectors.cpp
)
target_include_directories(straylight-timeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-timeline PRIVATE straylight-common)
install(TARGETS straylight-timeline DESTINATION bin)
