add_executable(straylight-alice-cli main.cpp)
target_link_libraries(straylight-alice-cli PRIVATE straylight-common)
install(TARGETS straylight-alice-cli DESTINATION bin)
