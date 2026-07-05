# Subsystem daemon tests — populated by Plan 2 chunks

find_package(OpenSSL REQUIRED)

add_executable(test_bus test_bus.cpp
    ${CMAKE_SOURCE_DIR}/bin/bus/bus_daemon.cpp)
target_link_libraries(test_bus PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_bus PRIVATE ${CMAKE_SOURCE_DIR}/bin/bus)
gtest_discover_tests(test_bus)

add_executable(test_photonics test_photonics.cpp
    ${CMAKE_SOURCE_DIR}/bin/photonics/mzi.cpp
    ${CMAKE_SOURCE_DIR}/bin/photonics/mesh.cpp
    ${CMAKE_SOURCE_DIR}/bin/photonics/detector.cpp)
target_link_libraries(test_photonics PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_photonics PRIVATE ${CMAKE_SOURCE_DIR}/bin/photonics)
gtest_discover_tests(test_photonics)

add_executable(test_registry test_registry.cpp
    ${CMAKE_SOURCE_DIR}/bin/registry/store.cpp)
target_link_libraries(test_registry PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_registry PRIVATE ${CMAKE_SOURCE_DIR}/bin/registry)
gtest_discover_tests(test_registry)

add_executable(test_entropy_daemon test_entropy_daemon.cpp
    ${CMAKE_SOURCE_DIR}/bin/entropy/drbg.cpp)
target_link_libraries(test_entropy_daemon PRIVATE
    straylight-common
    straylight-hw
    OpenSSL::Crypto
    GTest::gtest_main)
target_include_directories(test_entropy_daemon PRIVATE ${CMAKE_SOURCE_DIR}/bin/entropy)
gtest_discover_tests(test_entropy_daemon)

add_executable(test_scheduler test_scheduler.cpp
    ${CMAKE_SOURCE_DIR}/bin/scheduler/topology.cpp
    ${CMAKE_SOURCE_DIR}/bin/scheduler/cgroup.cpp)
target_link_libraries(test_scheduler PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_scheduler PRIVATE ${CMAKE_SOURCE_DIR}/bin/scheduler)
gtest_discover_tests(test_scheduler)

add_executable(test_core test_core.cpp
    ${CMAKE_SOURCE_DIR}/bin/core/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/bin/core/doctor.cpp
    ${CMAKE_SOURCE_DIR}/bin/core/core_daemon.cpp)
target_link_libraries(test_core PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_core PRIVATE ${CMAKE_SOURCE_DIR}/bin/core)
gtest_discover_tests(test_core)

add_executable(test_agent_queue test_agent_queue.cpp
    ${CMAKE_SOURCE_DIR}/bin/agent/task_queue.cpp)
target_link_libraries(test_agent_queue PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_agent_queue PRIVATE ${CMAKE_SOURCE_DIR}/bin/agent)
gtest_discover_tests(test_agent_queue)

add_executable(test_compiler test_compiler.cpp
    ${CMAKE_SOURCE_DIR}/bin/compiler/ir/graph.cpp
    ${CMAKE_SOURCE_DIR}/bin/compiler/ir/passes.cpp
    ${CMAKE_SOURCE_DIR}/bin/compiler/ir/lowering.cpp
    ${CMAKE_SOURCE_DIR}/bin/compiler/cache.cpp)
target_link_libraries(test_compiler PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_compiler PRIVATE ${CMAKE_SOURCE_DIR}/bin/compiler)
gtest_discover_tests(test_compiler)

add_executable(test_morph test_morph.cpp
    ${CMAKE_SOURCE_DIR}/bin/morph/quantize.cpp
    ${CMAKE_SOURCE_DIR}/bin/morph/prune.cpp
    ${CMAKE_SOURCE_DIR}/bin/morph/adapt.cpp)
target_link_libraries(test_morph PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_morph PRIVATE ${CMAKE_SOURCE_DIR}/bin/morph)
gtest_discover_tests(test_morph)

add_executable(test_snn test_snn.cpp
    ${CMAKE_SOURCE_DIR}/bin/snn/neuron.cpp
    ${CMAKE_SOURCE_DIR}/bin/snn/network.cpp
    ${CMAKE_SOURCE_DIR}/bin/snn/plasticity.cpp
    ${CMAKE_SOURCE_DIR}/bin/snn/simulator.cpp)
target_link_libraries(test_snn PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_snn PRIVATE ${CMAKE_SOURCE_DIR}/bin/snn)
gtest_discover_tests(test_snn)

add_executable(test_rhem test_rhem.cpp
    ${CMAKE_SOURCE_DIR}/bin/rhem/discovery.cpp
    ${CMAKE_SOURCE_DIR}/bin/rhem/allocator.cpp
    ${CMAKE_SOURCE_DIR}/bin/rhem/migration.cpp
    ${CMAKE_SOURCE_DIR}/bin/rhem/policy.cpp)
target_link_libraries(test_rhem PRIVATE straylight-common GTest::gtest_main)
target_include_directories(test_rhem PRIVATE ${CMAKE_SOURCE_DIR}/bin/rhem)
gtest_discover_tests(test_rhem)

# --- Plan 6: Network subsystems ---

find_package(PkgConfig QUIET)

# XDP tests
if(PkgConfig_FOUND)
    pkg_check_modules(LIBBPF QUIET libbpf)
endif()
if(LIBBPF_FOUND)
    add_executable(test_xdp test_xdp.cpp
        ${CMAKE_SOURCE_DIR}/bin/xdp/loader.cpp
        ${CMAKE_SOURCE_DIR}/bin/xdp/maps.cpp
        ${CMAKE_SOURCE_DIR}/bin/xdp/af_xdp.cpp)
    target_link_libraries(test_xdp PRIVATE straylight-common ${LIBBPF_LIBRARIES} GTest::gtest_main)
    target_include_directories(test_xdp PRIVATE
        ${CMAKE_SOURCE_DIR}/bin/xdp
        ${LIBBPF_INCLUDE_DIRS})
    gtest_discover_tests(test_xdp)
endif()

# DPDK tests
if(PkgConfig_FOUND)
    pkg_check_modules(DPDK QUIET libdpdk)
endif()
if(DPDK_FOUND)
    add_executable(test_dpdk test_dpdk.cpp
        ${CMAKE_SOURCE_DIR}/bin/dpdk/port.cpp
        ${CMAKE_SOURCE_DIR}/bin/dpdk/pipeline.cpp
        ${CMAKE_SOURCE_DIR}/bin/dpdk/flow.cpp
        ${CMAKE_SOURCE_DIR}/bin/dpdk/tensor_transport.cpp)
    target_link_libraries(test_dpdk PRIVATE straylight-common ${DPDK_LIBRARIES} GTest::gtest_main)
    target_include_directories(test_dpdk PRIVATE
        ${CMAKE_SOURCE_DIR}/bin/dpdk
        ${DPDK_INCLUDE_DIRS})
    target_compile_options(test_dpdk PRIVATE ${DPDK_CFLAGS_OTHER})
    gtest_discover_tests(test_dpdk)
endif()

# RDMA tests
if(PkgConfig_FOUND)
    pkg_check_modules(IBVERBS QUIET libibverbs)
endif()
if(IBVERBS_FOUND)
    add_executable(test_rdma test_rdma.cpp
        ${CMAKE_SOURCE_DIR}/bin/rdma_bus/verbs.cpp
        ${CMAKE_SOURCE_DIR}/bin/rdma_bus/memory_region.cpp
        ${CMAKE_SOURCE_DIR}/bin/rdma_bus/queue_pair.cpp
        ${CMAKE_SOURCE_DIR}/bin/rdma_bus/tensor_rdma.cpp)
    target_link_libraries(test_rdma PRIVATE straylight-common ${IBVERBS_LIBRARIES} ibverbs GTest::gtest_main)
    target_include_directories(test_rdma PRIVATE
        ${CMAKE_SOURCE_DIR}/bin/rdma_bus
        ${IBVERBS_INCLUDE_DIRS})
    gtest_discover_tests(test_rdma)
endif()

# --- Plan 7B: Enclave and FUSE subsystems ---

find_package(OpenSSL QUIET)

# Enclave attestation + sealed storage tests (no SGX hardware required)
if(OpenSSL_FOUND)
    add_executable(test_enclave_attestation
        test_enclave_attestation.cpp
        ${CMAKE_SOURCE_DIR}/bin/enclave/attestation.cpp
        ${CMAKE_SOURCE_DIR}/bin/enclave/sealed_storage.cpp)
    target_link_libraries(test_enclave_attestation PRIVATE
        straylight-common
        straylight-hw
        OpenSSL::Crypto
        GTest::gtest_main)
    target_include_directories(test_enclave_attestation PRIVATE
        ${CMAKE_SOURCE_DIR}/bin/enclave)
    gtest_discover_tests(test_enclave_attestation)
endif()

# FUSE compression tests (uses internal LZ77 codec — no libfuse3 needed)
add_executable(test_fuse_compression
    test_fuse_compression.cpp
    ${CMAKE_SOURCE_DIR}/bin/fuse/compression.cpp)
target_link_libraries(test_fuse_compression PRIVATE
    straylight-common
    GTest::gtest_main)
target_include_directories(test_fuse_compression PRIVATE
    ${CMAKE_SOURCE_DIR}/bin/fuse)
gtest_discover_tests(test_fuse_compression)
