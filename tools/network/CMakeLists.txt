cmake_minimum_required(VERSION 3.25)

add_executable(straylight-network
    main.cpp
    network_manager.cpp
)

target_compile_features(straylight-network PRIVATE cxx_std_20)

target_include_directories(straylight-network PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(straylight-network PRIVATE straylight-common)

# Linux requires no extra link flags for the POSIX socket/network headers;
# they are part of glibc.  On macOS (dev diagnostic builds only) the same
# headers are available via the system SDK.
if(UNIX AND NOT APPLE)
    # Explicitly request POSIX.1-2008 extensions on Linux.
    target_compile_definitions(straylight-network PRIVATE _GNU_SOURCE)
endif()

install(TARGETS straylight-network DESTINATION bin)
