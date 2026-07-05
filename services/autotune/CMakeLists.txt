add_executable(straylight-autotune
    main.cpp
    autotune_daemon.cpp
    tuner.cpp
    profiles.cpp
    hardware_probe.cpp
)
target_link_libraries(straylight-autotune PRIVATE straylight-common)
target_include_directories(straylight-autotune PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-autotune DESTINATION bin)
