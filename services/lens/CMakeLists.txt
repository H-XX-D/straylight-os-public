add_executable(straylight-lens
    main.cpp
    trace_collector.cpp
    correlator.cpp
    trace_store.cpp
)
target_link_libraries(straylight-lens PRIVATE straylight-common)
target_include_directories(straylight-lens PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-lens DESTINATION bin)
