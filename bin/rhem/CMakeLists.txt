add_executable(straylight-rhem
    main.cpp
    discovery.cpp
    allocator.cpp
    migration.cpp
    policy.cpp)
target_link_libraries(straylight-rhem PRIVATE straylight-common)
target_include_directories(straylight-rhem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-rhem DESTINATION bin)
