add_executable(straylight-vault
    main.cpp
    vault_store.cpp
    crypto.cpp
)
target_include_directories(straylight-vault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-vault PRIVATE straylight-common)

find_package(OpenSSL REQUIRED)
target_link_libraries(straylight-vault PRIVATE OpenSSL::SSL OpenSSL::Crypto)

install(TARGETS straylight-vault DESTINATION bin)
