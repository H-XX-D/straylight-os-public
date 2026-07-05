add_executable(straylight-secrets
    main.cpp
    secrets_engine.cpp
)
find_package(OpenSSL REQUIRED)
target_include_directories(straylight-secrets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-secrets PRIVATE straylight-common OpenSSL::Crypto)
install(TARGETS straylight-secrets DESTINATION bin)
install(DIRECTORY etc/ DESTINATION etc)
