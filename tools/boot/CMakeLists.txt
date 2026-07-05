add_executable(straylight-boot
    main.cpp
    boot_manager.cpp
)
target_include_directories(straylight-boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-boot PRIVATE straylight-common)
install(TARGETS straylight-boot DESTINATION bin)
