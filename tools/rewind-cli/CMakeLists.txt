cmake_minimum_required(VERSION 3.25)
project(rewind-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(rewind-cli main.cpp)

install(TARGETS rewind-cli DESTINATION bin)
