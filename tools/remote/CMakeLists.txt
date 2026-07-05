add_executable(straylight-remote
    main.cpp
    tls_client.cpp
    terminal.cpp
)

target_include_directories(straylight-remote PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(zstd REQUIRED)

target_link_libraries(straylight-remote PRIVATE
    straylight-common
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    zstd::libzstd_shared
    Threads::Threads
)

install(TARGETS straylight-remote DESTINATION bin)
