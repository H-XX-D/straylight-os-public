add_executable(straylight-migrate
    main.cpp
    migrator.cpp
    diff_engine.cpp
)
target_include_directories(straylight-migrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-migrate PRIVATE straylight-common)

find_package(OpenSSL REQUIRED)
target_link_libraries(straylight-migrate PRIVATE OpenSSL::Crypto)

install(TARGETS straylight-migrate DESTINATION bin)
