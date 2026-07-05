add_executable(straylight-cron-cli main.cpp)
target_link_libraries(straylight-cron-cli PRIVATE straylight-common)
install(TARGETS straylight-cron-cli DESTINATION bin)
