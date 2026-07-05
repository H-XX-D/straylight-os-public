add_executable(straylight-compiler
    main.cpp
    ir/graph.cpp
    ir/passes.cpp
    ir/lowering.cpp
    cache.cpp
)

target_link_libraries(straylight-compiler PRIVATE straylight-common)
target_include_directories(straylight-compiler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-compiler DESTINATION bin)
