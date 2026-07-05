add_executable(straylight-pmem
    main.cpp
    dax.cpp
    allocator.cpp
    log.cpp
    checkpoint.cpp)
target_link_libraries(straylight-pmem PRIVATE straylight-common)
target_include_directories(straylight-pmem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-pmem DESTINATION bin)
