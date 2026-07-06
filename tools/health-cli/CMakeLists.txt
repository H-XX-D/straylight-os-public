add_executable(straylight-health main.cpp)
target_link_libraries(straylight-health PRIVATE straylight-common)
install(TARGETS straylight-health DESTINATION bin)
install(CODE "
    execute_process(
        COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
                \"straylight-health\"
                \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/straylight-health-cli\"
    )
")
