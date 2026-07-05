add_executable(straylight-dns
    main.cpp
    dns_manager.cpp
)
target_include_directories(straylight-dns PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-dns PRIVATE straylight-common)
install(TARGETS straylight-dns DESTINATION bin)
