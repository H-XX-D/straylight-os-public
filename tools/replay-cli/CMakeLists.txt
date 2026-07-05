add_executable(straylight-replay-cli main.cpp)
target_link_libraries(straylight-replay-cli PRIVATE straylight-common)
install(TARGETS straylight-replay-cli DESTINATION bin)
