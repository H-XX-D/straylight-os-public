add_executable(straylight-weave-cli
    main.cpp
)
target_include_directories(straylight-weave-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-weave-cli PRIVATE straylight-common)
install(TARGETS straylight-weave-cli DESTINATION bin)
