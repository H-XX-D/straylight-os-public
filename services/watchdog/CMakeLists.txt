add_executable(straylight-watchdog
    main.cpp
    watchdog_engine.cpp
)
target_include_directories(straylight-watchdog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-watchdog PRIVATE straylight-common)
install(TARGETS straylight-watchdog DESTINATION bin)
install(DIRECTORY etc/ DESTINATION etc)
