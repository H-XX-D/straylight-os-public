add_executable(straylight-nerve
    main.cpp
    irq_mapper.cpp
    affinity_optimizer.cpp
    irq_monitor.cpp
)
target_include_directories(straylight-nerve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-nerve PRIVATE straylight-common)
install(TARGETS straylight-nerve DESTINATION bin)
