add_executable(straylight-weave
    main.cpp
    pipeline_engine.cpp
    pipeline_parser.cpp
    node_registry.cpp
)
target_include_directories(straylight-weave PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-weave PRIVATE straylight-common)
install(TARGETS straylight-weave DESTINATION bin)
