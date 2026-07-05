add_executable(straylight-splice
    main.cpp
    splice_engine.cpp
)
target_link_libraries(straylight-splice PRIVATE straylight-common)
target_include_directories(straylight-splice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-splice DESTINATION bin)
