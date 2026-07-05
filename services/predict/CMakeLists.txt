cmake_minimum_required(VERSION 3.25)
project(straylight-predict LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(straylight-predict
    main.cpp
    prediction_engine.cpp
    preloader.cpp
    usage_collector.cpp
)

target_include_directories(straylight-predict PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(straylight-predict PRIVATE straylight-common Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-predict PRIVATE rt)
endif()

install(TARGETS straylight-predict RUNTIME DESTINATION bin)
