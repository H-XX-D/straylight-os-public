add_executable(straylight-sandbox
    main.cpp
    sandbox_manager.cpp
    namespace_setup.cpp
)
target_include_directories(straylight-sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-sandbox PRIVATE straylight-common)
install(TARGETS straylight-sandbox DESTINATION bin)
