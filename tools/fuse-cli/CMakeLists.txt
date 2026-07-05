cmake_minimum_required(VERSION 3.25)
project(straylight-fuse-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-fuse-cli main.cpp)

target_include_directories(straylight-fuse-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

install(TARGETS straylight-fuse-cli RUNTIME DESTINATION bin)
