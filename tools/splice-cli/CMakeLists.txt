add_executable(straylight-splice-cli main.cpp)
target_link_libraries(straylight-splice-cli PRIVATE straylight-common)
install(TARGETS straylight-splice-cli DESTINATION bin)
