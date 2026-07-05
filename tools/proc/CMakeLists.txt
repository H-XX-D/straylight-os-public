add_executable(straylight-proc
    main.cpp
    proc_inspector.cpp
)
target_include_directories(straylight-proc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-proc PRIVATE straylight-common)
install(TARGETS straylight-proc DESTINATION bin)
