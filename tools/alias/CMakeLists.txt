add_executable(straylight-alias
    main.cpp
    alias_manager.cpp
)
target_include_directories(straylight-alias PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-alias PRIVATE straylight-common)
install(TARGETS straylight-alias DESTINATION bin)
