cmake_minimum_required(VERSION 3.25)
project(straylight-hotpatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-hotpatchd
    main.cpp
    patch_engine.cpp
)

target_include_directories(straylight-hotpatchd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(straylight-hotpatchd PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-hotpatchd PRIVATE rt)
endif()

install(TARGETS straylight-hotpatchd RUNTIME DESTINATION sbin)
