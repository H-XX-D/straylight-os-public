cmake_minimum_required(VERSION 3.25)
project(straylight-hotpatch-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-hotpatch main.cpp)

target_include_directories(straylight-hotpatch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

install(TARGETS straylight-hotpatch RUNTIME DESTINATION bin)
