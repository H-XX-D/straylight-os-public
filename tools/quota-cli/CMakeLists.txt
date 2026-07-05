cmake_minimum_required(VERSION 3.25)
project(straylight-quota-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-quota main.cpp)

target_include_directories(straylight-quota PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

install(TARGETS straylight-quota RUNTIME DESTINATION bin)
