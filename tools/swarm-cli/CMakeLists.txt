add_executable(straylight-swarm-cli main.cpp)
target_include_directories(straylight-swarm-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-swarm-cli DESTINATION bin)
