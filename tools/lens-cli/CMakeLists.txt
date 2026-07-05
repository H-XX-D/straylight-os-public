add_executable(straylight-lens-cli main.cpp)
target_link_libraries(straylight-lens-cli PRIVATE straylight-common)
install(TARGETS straylight-lens-cli DESTINATION bin)
