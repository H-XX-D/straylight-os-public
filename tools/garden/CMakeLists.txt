add_executable(straylight-garden
    main.cpp
    garden_manager.cpp
    environment.cpp
)

target_include_directories(straylight-garden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-garden PRIVATE straylight-common)

install(TARGETS straylight-garden DESTINATION bin)
