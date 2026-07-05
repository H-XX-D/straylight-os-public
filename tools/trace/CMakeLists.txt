add_executable(straylight-trace
    main.cpp
    tracer.cpp
    report.cpp
)

target_include_directories(straylight-trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-trace PRIVATE straylight-common)

install(TARGETS straylight-trace DESTINATION bin)
