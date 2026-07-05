cmake_minimum_required(VERSION 3.25)
project(straylight-thermal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(straylight-thermal
    main.cpp
    thermal_model.cpp
    throttle_controller.cpp
    thermal_log.cpp
)

target_include_directories(straylight-thermal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(straylight-thermal PRIVATE straylight-common Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(straylight-thermal PRIVATE rt)
endif()

install(TARGETS straylight-thermal RUNTIME DESTINATION bin)
