add_executable(straylight-notify
    main.cpp
    notification_engine.cpp
    dbus_bridge.cpp
)
target_include_directories(straylight-notify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(straylight-notify PRIVATE straylight-common systemd)
install(TARGETS straylight-notify DESTINATION bin)
