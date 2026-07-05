add_executable(straylight-app-cli
    main.cpp
)

target_include_directories(straylight-app-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS straylight-app-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(STRAYLIGHT_APP_CLI_SYMLINKS
    straylight-terminal-cli
    straylight-files-cli
    straylight-sysmon-cli
    straylight-settings-cli
    straylight-oobe-cli
    straylight-wizard-cli
    straylight-widget-launcher-cli
    straylight-audio-gui-cli
    straylight-audio-mixer-cli
    straylight-bench-gui-cli
    straylight-boot-gui-cli
    straylight-color-gui-cli
    straylight-cron-gui-cli
    straylight-disk-gui-cli
    straylight-display-gui-cli
    straylight-fabric-gui-cli
    straylight-flux-gui-cli
    straylight-fonts-gui-cli
    straylight-health-gui-cli
    straylight-hub-cli
    straylight-input-gui-cli
    straylight-intent-gui-cli
    straylight-logview-gui-cli
    straylight-mesh-gui-cli
    straylight-migrate-gui-cli
    straylight-morph-gui-cli
    straylight-network-gui-cli
    straylight-notify-gui-cli
    straylight-photonics-gui-cli
    straylight-pmem-gui-cli
    straylight-policy-gui-cli
    straylight-power-gui-cli
    straylight-predict-gui-cli
    straylight-probe-gui-cli
    straylight-quantum-gui-cli
    straylight-replay-gui-cli
    straylight-rewind-gui-cli
    straylight-rhem-gui-cli
    straylight-sandbox-gui-cli
    straylight-shield-gui-cli
    straylight-snapshot-gui-cli
    straylight-snn-gui-cli
    straylight-update-gui-cli
    straylight-users-gui-cli
    straylight-vault-gui-cli
)

foreach(cli_name IN LISTS STRAYLIGHT_APP_CLI_SYMLINKS)
    install(CODE
        "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
         execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"straylight-app-cli\" \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${cli_name}\")"
    )
endforeach()
