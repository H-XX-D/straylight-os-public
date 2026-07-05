add_executable(straylight-backup
    main.cpp
    backup_engine.cpp
)
target_include_directories(straylight-backup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-backup PRIVATE straylight-common)
install(TARGETS straylight-backup DESTINATION bin)
install(DIRECTORY etc/ DESTINATION etc)
