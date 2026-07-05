add_executable(straylight-flux-cli main.cpp)
target_link_libraries(straylight-flux-cli PRIVATE straylight-common)
install(TARGETS straylight-flux-cli DESTINATION bin)
