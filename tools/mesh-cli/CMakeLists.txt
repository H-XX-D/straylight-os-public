add_executable(straylight-mesh-cli
    main.cpp
)
target_link_libraries(straylight-mesh-cli PRIVATE straylight-common)
target_include_directories(straylight-mesh-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(straylight-mesh-cli PRIVATE cxx_std_20)
set_target_properties(straylight-mesh-cli PROPERTIES OUTPUT_NAME straylight-mesh)
install(TARGETS straylight-mesh-cli DESTINATION bin)
