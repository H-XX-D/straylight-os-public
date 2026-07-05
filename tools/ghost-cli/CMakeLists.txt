add_executable(straylight-ghost-cli main.cpp)
target_link_libraries(straylight-ghost-cli PRIVATE straylight-common)
install(TARGETS straylight-ghost-cli DESTINATION bin)
