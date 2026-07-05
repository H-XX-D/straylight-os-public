add_executable(straylight-policy
    main.cpp
    policy_engine.cpp
    role_config.cpp
    compliance_checker.cpp
)
target_link_libraries(straylight-policy PRIVATE straylight-common)
target_include_directories(straylight-policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-policy DESTINATION bin)
