add_executable(straylight-bench
    main.cpp
    benchmarks.cpp
    report.cpp
)

target_include_directories(straylight-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-bench PRIVATE straylight-common)
install(TARGETS straylight-bench DESTINATION bin)
