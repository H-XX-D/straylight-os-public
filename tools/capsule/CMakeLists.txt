add_executable(straylight-capsule
    main.cpp
    capsule_manifest.cpp
    capsule_builder.cpp
    capsule_installer.cpp
    capsule_runner.cpp
)
target_include_directories(straylight-capsule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-capsule PRIVATE straylight-common)
install(TARGETS straylight-capsule DESTINATION bin)
