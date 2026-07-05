add_executable(straylight-shield
    main.cpp
    auditor.cpp
    hardener.cpp
)
target_include_directories(straylight-shield PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-shield PRIVATE straylight-common)
install(TARGETS straylight-shield DESTINATION bin)
