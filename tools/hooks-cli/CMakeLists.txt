add_executable(straylight-hooks-cli
    main.cpp
)
target_include_directories(straylight-hooks-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-hooks-cli PRIVATE straylight-common)
install(TARGETS straylight-hooks-cli DESTINATION bin)
