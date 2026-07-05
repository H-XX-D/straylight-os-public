# bin/enclave/CMakeLists.txt
# straylight-enclave — SGX attestation, sealed storage, and secure inference tool.
# Requires OpenSSL. SGX SDK is optional; stub fallback is always available.

find_package(OpenSSL REQUIRED)

add_executable(straylight-enclave
    main.cpp
    attestation.cpp
    sealed_storage.cpp
    secure_inference.cpp)

target_include_directories(straylight-enclave
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(straylight-enclave
    PRIVATE
        straylight-common
        straylight-ml
        straylight-hw
        OpenSSL::Crypto)

# Optional Intel SGX SDK — graceful stub fallback when unavailable.
find_package(SGX QUIET)
if(SGX_FOUND)
    target_compile_definitions(straylight-enclave PRIVATE STRAYLIGHT_SGX_HW=1)
    target_link_libraries(straylight-enclave PRIVATE SGX::urts SGX::uae_service)
    message(STATUS "straylight-enclave: SGX hardware mode enabled")
else()
    message(STATUS "straylight-enclave: SGX not found, using software stub")
endif()

install(TARGETS straylight-enclave RUNTIME DESTINATION bin)
