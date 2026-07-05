cmake_minimum_required(VERSION 3.25)
project(straylight-predict-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-predict-cli main.cpp)

# The CLI is a standalone socket client — no library dependencies needed
find_package(Threads REQUIRED)
target_link_libraries(straylight-predict-cli PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-predict-cli PRIVATE rt)
endif()

set_target_properties(straylight-predict-cli PROPERTIES
    OUTPUT_NAME straylight-predict-cli
)

install(TARGETS straylight-predict-cli RUNTIME DESTINATION bin)
