cmake_minimum_required(VERSION 3.25)
project(straylight-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(straylight-bridge
    main.cpp
    bridge_engine.cpp
    page_tracker.cpp
    network_sync.cpp
)

target_include_directories(straylight-bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../straylight-os/include
)

target_link_libraries(straylight-bridge PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-bridge PRIVATE rt)
endif()

if(APPLE)
    # macOS doesn't have -lrt but has shm_open in libSystem
endif()

install(TARGETS straylight-bridge RUNTIME DESTINATION bin)
