add_executable(straylight-perf
    main.cpp
    perf_counter.cpp
)
target_include_directories(straylight-perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-perf PRIVATE straylight-common)
install(TARGETS straylight-perf DESTINATION bin)
