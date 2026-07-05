add_executable(straylight-quantum
    main.cpp
    state_vector.cpp
    gates.cpp
    circuit.cpp
    noise.cpp
    measure.cpp)
target_link_libraries(straylight-quantum PRIVATE straylight-common)
target_include_directories(straylight-quantum PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS straylight-quantum DESTINATION bin)
