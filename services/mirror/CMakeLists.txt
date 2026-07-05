cmake_minimum_required(VERSION 3.25)
project(straylight-mirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(straylight-mirror
    main.cpp
    mirror_engine.cpp
    state_capture.cpp
    transfer.cpp
)

target_include_directories(straylight-mirror PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../straylight-os/include
)

target_link_libraries(straylight-mirror PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-mirror PRIVATE rt)
endif()

install(TARGETS straylight-mirror RUNTIME DESTINATION bin)
