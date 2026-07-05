add_executable(straylight-locale
    main.cpp
    locale_manager.cpp
)
target_include_directories(straylight-locale PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-locale PRIVATE straylight-common)
install(TARGETS straylight-locale DESTINATION bin)
