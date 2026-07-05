add_executable(straylight-hooks
    main.cpp
    hook_engine.cpp
)
target_include_directories(straylight-hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-hooks PRIVATE straylight-common)
install(TARGETS straylight-hooks DESTINATION bin)
install(DIRECTORY etc/ DESTINATION etc)
