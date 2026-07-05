add_executable(straylight-cron
    main.cpp
    scheduler.cpp
    task.cpp
)
target_include_directories(straylight-cron PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-cron PRIVATE straylight-common)
install(TARGETS straylight-cron DESTINATION bin)
