cmake_minimum_required(VERSION 3.25)
project(mirror-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(mirror-cli
    main.cpp
    ../../services/mirror/mirror_engine.cpp
    ../../services/mirror/state_capture.cpp
    ../../services/mirror/transfer.cpp
)

target_include_directories(mirror-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../services/mirror
    ${CMAKE_CURRENT_SOURCE_DIR}/../../straylight-os/include
)

target_link_libraries(mirror-cli PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mirror-cli PRIVATE rt)
endif()

install(TARGETS mirror-cli RUNTIME DESTINATION bin)
