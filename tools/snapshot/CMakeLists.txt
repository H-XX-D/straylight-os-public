add_executable(straylight-snapshot
    main.cpp
    snapshot_manager.cpp
)
target_include_directories(straylight-snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-snapshot PRIVATE straylight-common)
install(TARGETS straylight-snapshot DESTINATION bin)
