add_executable(straylight-power-cli
    main.cpp
    ${CMAKE_SOURCE_DIR}/services/power/power_engine.cpp
)
target_include_directories(straylight-power-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/services/power
)
target_link_libraries(straylight-power-cli PRIVATE straylight-common)
install(TARGETS straylight-power-cli DESTINATION bin)
