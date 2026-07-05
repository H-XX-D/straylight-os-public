add_executable(straylight-core main.cpp core_daemon.cpp pipeline.cpp doctor.cpp)
target_link_libraries(straylight-core PRIVATE straylight-common)
target_include_directories(straylight-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-core DESTINATION bin)
