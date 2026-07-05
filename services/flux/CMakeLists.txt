add_executable(straylight-flux
    main.cpp
    stream_engine.cpp
    filters.cpp
)

target_include_directories(straylight-flux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-flux PRIVATE straylight-common)

install(TARGETS straylight-flux DESTINATION bin)
