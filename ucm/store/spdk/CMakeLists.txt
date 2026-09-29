set(SPDK_ROOT_DIR "$ENV{SPDK_ROOT_DIR}" CACHE PATH "SPDK source tree")
if(NOT SPDK_ROOT_DIR)
    find_path(SPDK_AUTODETECT_ROOT
        NAMES include/spdk/nvme.h
        HINTS
            "${UCM_ROOT_DIR}/../spdk"
            "${UCM_ROOT_DIR}/../../devSPDK/spdk"
    )
    if(SPDK_AUTODETECT_ROOT)
        set(SPDK_ROOT_DIR "${SPDK_AUTODETECT_ROOT}" CACHE PATH "SPDK source tree" FORCE)
    endif()
endif()
if(NOT SPDK_ROOT_DIR)
    message(STATUS "spdkstore: skipping build - set SPDK_ROOT_DIR to an SPDK source tree")
    return()
endif()
set(SPDK_BUILD_DIR "${SPDK_ROOT_DIR}/build")
set(SPDK_DPDK_LIB_DIR "${SPDK_ROOT_DIR}/dpdk/build/lib")
set(SPDK_ENGINE_SOURCE
    "${UCM_ROOT_DIR}/testSPDK/ucm_spdk_store/ucm_spdk_store.c")

if(NOT EXISTS "${SPDK_BUILD_DIR}/lib/libspdk_nvme.a")
    message(STATUS "spdkstore: skipping build - SPDK is not built under ${SPDK_ROOT_DIR}")
    return()
endif()

add_library(ucm_spdk_engine STATIC
    ${SPDK_ENGINE_SOURCE}
    cc/engine_shim.c
)
set_source_files_properties(${SPDK_ENGINE_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS ENGINE_LIB=1
)
set_target_properties(ucm_spdk_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)
target_compile_options(ucm_spdk_engine PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable
)
target_include_directories(ucm_spdk_engine PRIVATE
    ${SPDK_ROOT_DIR}/include
    ${SPDK_ROOT_DIR}/lib/nvme
    ${SPDK_ROOT_DIR}/lib/env_dpdk
)

set(SPDK_STATIC_LIBS
    spdk_sock_posix spdk_nvme spdk_keyring spdk_sock spdk_trace spdk_rpc
    spdk_jsonrpc spdk_json spdk_rdma_provider spdk_rdma_utils spdk_dma
    spdk_rdma_cm spdk_util spdk_log spdk_env_dpdk
)
set(DPDK_STATIC_LIBS
    rte_argparse rte_bus_pci rte_cryptodev rte_dmadev rte_eal rte_ethdev
    rte_hash rte_kvargs rte_log rte_mbuf rte_mempool rte_mempool_ring
    rte_meter rte_net rte_pci rte_power rte_power_acpi rte_power_amd_pstate
    rte_power_cppc rte_power_intel_pstate rte_power_intel_uncore
    rte_power_kvm_vm rte_rcu rte_ring rte_telemetry rte_timer rte_vhost
)
set(SPDK_STATIC_ARCHIVES)
foreach(LIB IN LISTS SPDK_STATIC_LIBS)
    list(APPEND SPDK_STATIC_ARCHIVES "${SPDK_BUILD_DIR}/lib/lib${LIB}.a")
endforeach()
set(DPDK_STATIC_ARCHIVES)
foreach(LIB IN LISTS DPDK_STATIC_LIBS)
    list(APPEND DPDK_STATIC_ARCHIVES "${SPDK_DPDK_LIB_DIR}/lib${LIB}.a")
endforeach()

file(GLOB_RECURSE UCM_SPDK_STORE_CC_SOURCE_FILES "./cc/*.cc")
add_library(spdkstore SHARED ${UCM_SPDK_STORE_CC_SOURCE_FILES})
target_include_directories(spdkstore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cc
    ${UCM_ROOT_DIR}/ucm/store/detail/type
    ${UCM_ROOT_DIR}/ucm/shared/infra/logger
    ${UCM_ROOT_DIR}/ucm/shared/infra/status
    ${UCM_ROOT_DIR}/ucm/shared/metrics
    ${SPDK_ROOT_DIR}/include
)
target_link_directories(spdkstore PRIVATE
    ${SPDK_BUILD_DIR}/lib
    ${SPDK_DPDK_LIB_DIR}
)
target_link_libraries(spdkstore PRIVATE
    storeintf infra_logger metrics ucm_spdk_engine
    -Wl,--whole-archive
    ${SPDK_STATIC_ARCHIVES}
    ${DPDK_STATIC_ARCHIVES}
    -Wl,--no-whole-archive
    ${SPDK_ROOT_DIR}/isa-l/.libs/libisal.a
    ${SPDK_ROOT_DIR}/isa-l-crypto/.libs/libisal_crypto.a
    numa dl ibverbs rdmacm pthread rt uuid ssl crypto m fuse3 keyutils aio z
)
set_target_properties(spdkstore PROPERTIES
    INSTALL_RPATH "$ORIGIN/../../shared/metrics"
)

file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS spdkstore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
