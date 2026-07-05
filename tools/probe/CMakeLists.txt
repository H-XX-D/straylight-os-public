add_executable(straylight-probe
    main.cpp
)
target_include_directories(straylight-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-probe PRIVATE straylight-common)
install(TARGETS straylight-probe DESTINATION bin)
