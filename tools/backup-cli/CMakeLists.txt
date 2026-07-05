add_executable(straylight-backup-cli
    main.cpp
)
target_include_directories(straylight-backup-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-backup-cli PRIVATE straylight-common)
install(TARGETS straylight-backup-cli DESTINATION bin)
