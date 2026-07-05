add_executable(straylight-profile
    main.cpp
    profiler.cpp
)
target_include_directories(straylight-profile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-profile PRIVATE straylight-common)
install(TARGETS straylight-profile DESTINATION bin)
