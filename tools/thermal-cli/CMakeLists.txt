cmake_minimum_required(VERSION 3.25)
project(thermal-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(thermal-cli
    main.cpp
    ../../services/thermal/thermal_model.cpp
    ../../services/thermal/throttle_controller.cpp
)

target_include_directories(thermal-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../services/thermal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../straylight-os/include
)

target_link_libraries(thermal-cli PRIVATE Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(thermal-cli PRIVATE rt)
endif()

install(TARGETS thermal-cli RUNTIME DESTINATION bin)
