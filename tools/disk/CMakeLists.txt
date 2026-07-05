add_executable(straylight-disk
    main.cpp
    disk_manager.cpp
)
target_include_directories(straylight-disk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-disk PRIVATE straylight-common)
install(TARGETS straylight-disk DESTINATION bin)
