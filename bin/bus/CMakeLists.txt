add_executable(straylight-bus main.cpp bus_daemon.cpp)
target_link_libraries(straylight-bus PRIVATE straylight-common)
target_include_directories(straylight-bus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-bus DESTINATION bin)
