add_executable(straylight-nerve-cli
    main.cpp
)
target_include_directories(straylight-nerve-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-nerve-cli PRIVATE straylight-common)
install(TARGETS straylight-nerve-cli DESTINATION bin)
