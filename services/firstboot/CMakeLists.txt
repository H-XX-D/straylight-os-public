cmake_minimum_required(VERSION 3.25)

# ── straylight-hwdetect binary ────────────────────────────────────────────────
add_executable(straylight-hwdetect
    hwdetect.cpp
    hwdetect_main.cpp
)

target_include_directories(straylight-hwdetect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/common/include
)

target_compile_features(straylight-hwdetect PRIVATE cxx_std_20)

find_package(nlohmann_json 3.11 REQUIRED)

target_link_libraries(straylight-hwdetect PRIVATE
    straylight-common
    nlohmann_json::nlohmann_json
)

install(TARGETS straylight-hwdetect
    RUNTIME DESTINATION lib/straylight
)

# ── firstboot shell scripts ───────────────────────────────────────────────────
install(PROGRAMS
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-firstboot
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-oobe-check
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-wizard-check
    DESTINATION lib/straylight
)

# ── systemd units ─────────────────────────────────────────────────────────────
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-firstboot.service
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-hwdetect.service
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-oobe.service
    ${CMAKE_CURRENT_SOURCE_DIR}/straylight-wizard.service
    DESTINATION lib/systemd/system
)

# ── udev rules ────────────────────────────────────────────────────────────────
set(STRAYLIGHT_UDEV_RULE_CANDIDATES
    ${CMAKE_SOURCE_DIR}/services/udev/90-straylight-gpu.rules
    ${CMAKE_SOURCE_DIR}/services/udev/90-straylight-sgx.rules
    ${CMAKE_SOURCE_DIR}/services/udev/90-straylight-pmem.rules
)

set(STRAYLIGHT_UDEV_RULES)
foreach(rule ${STRAYLIGHT_UDEV_RULE_CANDIDATES})
    if(EXISTS "${rule}")
        list(APPEND STRAYLIGHT_UDEV_RULES "${rule}")
    endif()
endforeach()

if(STRAYLIGHT_UDEV_RULES)
    install(FILES ${STRAYLIGHT_UDEV_RULES}
        DESTINATION lib/udev/rules.d
    )
endif()

# ── state directory (created at install time) ─────────────────────────────────
install(DIRECTORY DESTINATION /var/lib/straylight)
