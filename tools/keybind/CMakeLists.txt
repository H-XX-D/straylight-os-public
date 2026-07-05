add_executable(straylight-keybind
    main.cpp
    keybind_manager.cpp
)
target_include_directories(straylight-keybind PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-keybind PRIVATE straylight-common)
install(TARGETS straylight-keybind DESTINATION bin)
