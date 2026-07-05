add_executable(straylight-morph
    main.cpp
    quantize.cpp
    prune.cpp
    distill.cpp
    adapt.cpp)
target_link_libraries(straylight-morph PRIVATE straylight-common)
target_include_directories(straylight-morph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-morph DESTINATION bin)
