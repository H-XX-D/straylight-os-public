add_executable(straylight-health-cli main.cpp)
target_link_libraries(straylight-health-cli PRIVATE straylight-common)
install(TARGETS straylight-health-cli DESTINATION bin)
