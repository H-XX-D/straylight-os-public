add_executable(straylight-photonics
    main.cpp
    mzi.cpp
    mesh.cpp
    detector.cpp
    device.cpp)
target_link_libraries(straylight-photonics PRIVATE straylight-common)
target_include_directories(straylight-photonics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-photonics DESTINATION bin)
