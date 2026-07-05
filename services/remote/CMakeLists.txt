add_executable(straylight-remote-agent
    main.cpp
    agent.cpp
    tls_server.cpp
    command_handler.cpp
    file_transfer.cpp
    system_info.cpp
)

target_include_directories(straylight-remote-agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(zstd REQUIRED)

target_link_libraries(straylight-remote-agent PRIVATE
    straylight-common
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    zstd::libzstd_shared
    Threads::Threads
)

install(TARGETS straylight-remote-agent DESTINATION bin)
