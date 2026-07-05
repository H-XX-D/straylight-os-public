add_executable(straylight-journal
    main.cpp
    journal_engine.cpp
)
target_include_directories(straylight-journal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-journal PRIVATE straylight-common)
install(TARGETS straylight-journal DESTINATION bin)
