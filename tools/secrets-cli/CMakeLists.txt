add_executable(straylight-secrets-cli
    main.cpp
)
target_include_directories(straylight-secrets-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-secrets-cli PRIVATE straylight-common)
install(TARGETS straylight-secrets-cli DESTINATION bin)
