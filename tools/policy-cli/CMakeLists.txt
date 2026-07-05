add_executable(straylight-policy-cli main.cpp)
target_link_libraries(straylight-policy-cli PRIVATE straylight-common)
install(TARGETS straylight-policy-cli DESTINATION bin)
