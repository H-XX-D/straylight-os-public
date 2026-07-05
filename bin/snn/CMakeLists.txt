add_executable(straylight-snn
    main.cpp
    neuron.cpp
    network.cpp
    plasticity.cpp
    simulator.cpp)
target_link_libraries(straylight-snn PRIVATE straylight-common)
target_include_directories(straylight-snn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-snn DESTINATION bin)
