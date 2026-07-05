add_executable(straylight-power
    main.cpp
    power_engine.cpp
)
target_include_directories(straylight-power PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-power PRIVATE straylight-common)
install(TARGETS straylight-power DESTINATION bin)
