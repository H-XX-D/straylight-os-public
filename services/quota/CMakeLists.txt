cmake_minimum_required(VERSION 3.25)
project(straylight-quota LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-quota
    main.cpp
    quota_engine.cpp
)

target_include_directories(straylight-quota PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(straylight-quota PRIVATE straylight-common Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-quota PRIVATE rt)
endif()

install(TARGETS straylight-quota RUNTIME DESTINATION bin)
