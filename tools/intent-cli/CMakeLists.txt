cmake_minimum_required(VERSION 3.25)
project(straylight-intent-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-intent-cli main.cpp)

# The CLI is a standalone socket client — no library dependencies needed
find_package(Threads REQUIRED)
target_link_libraries(straylight-intent-cli PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-intent-cli PRIVATE rt)
endif()

set_target_properties(straylight-intent-cli PROPERTIES
    OUTPUT_NAME straylight-intent
)

install(TARGETS straylight-intent-cli RUNTIME DESTINATION bin)
