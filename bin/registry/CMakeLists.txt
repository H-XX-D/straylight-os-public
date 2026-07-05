add_executable(straylight-registry main.cpp registry_daemon.cpp store.cpp)
target_link_libraries(straylight-registry PRIVATE straylight-common)
target_include_directories(straylight-registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-registry DESTINATION bin)
