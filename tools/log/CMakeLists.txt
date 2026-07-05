add_executable(straylight-log
    main.cpp
    log_engine.cpp
)
target_include_directories(straylight-log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-log PRIVATE straylight-common)
install(TARGETS straylight-log DESTINATION bin)
