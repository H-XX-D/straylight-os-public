add_executable(straylight-ghost
    main.cpp
    migration_engine.cpp
    page_server.cpp
    process_restore.cpp
)
target_link_libraries(straylight-ghost PRIVATE straylight-common)
target_include_directories(straylight-ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-ghost DESTINATION bin)
