cmake_minimum_required(VERSION 3.20)
project(wlh_host_macos_sim VERSION 0.1.0 LANGUAGES C)

include(CTest)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

set(WLH_SIM_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(core/host-core core-host-build EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${WLH_SIM_BUILD_TESTING})
wlh_common_enable_posix_osal(BUILD_TESTING "${WLH_SIM_BUILD_TESTING}")
wlh_common_enable_log(BACKEND EASYLOGGER)

target_compile_definitions(wlh_host_core PRIVATE WLH_ENABLE_TEST_HOOKS)

add_library(wlh_easylogger STATIC
    third_party/easylogger/easylogger/src/elog.c
    third_party/easylogger/easylogger/src/elog_utils.c
    third_party/easylogger_port/elog_port.c)
add_library(wlh::easylogger ALIAS wlh_easylogger)
target_include_directories(wlh_easylogger PUBLIC
    third_party/easylogger_port
    third_party/easylogger/easylogger/inc)
target_compile_features(wlh_easylogger PUBLIC c_std_99)
set_target_properties(wlh_easylogger PROPERTIES
    C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_easylogger PRIVATE -Wall -Werror)
endif()

# The core log backend needs EasyLogger headers whenever a translation unit
# includes wlh/log.h with the EASYLOGGER backend selected.
target_link_libraries(wlh_log_backend PUBLIC wlh::easylogger)

set(LWIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip")
set(LWIP_EXCLUDE_SLIPIF TRUE)
include("${LWIP_DIR}/src/Filelists.cmake")
add_library(wlh_host_sim_lwip STATIC
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    "${LWIP_DIR}/src/api/tcpip.c"
    "${LWIP_DIR}/src/netif/ethernet.c"
    src/lwip_port/sys_arch.c)
target_include_directories(wlh_host_sim_lwip
    PUBLIC
        "${LWIP_DIR}/src/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lwip_port")
target_compile_features(wlh_host_sim_lwip PUBLIC c_std_11)
target_link_libraries(wlh_host_sim_lwip PUBLIC wlh::osal Threads::Threads)

add_library(wlh_host_sim_platform STATIC
    src/ipc.c src/executor.c src/transport_usb.c src/network.c)
target_include_directories(wlh_host_sim_platform
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(wlh_host_sim_platform PUBLIC c_std_11)
target_link_libraries(wlh_host_sim_platform
    PUBLIC
        Threads::Threads
        PkgConfig::LIBUSB
        wlh::log
        wlh_host_sim_lwip)

# --- BLE stack (NimBLE 1.10 + Mbed-TLS crypto) --------------------------------
# NimBLE's Security Manager unconditionally pulls in Mbed-TLS (AES/CMAC/ECDH),
# so we build the vendored crypto library and link it under the NimBLE host.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

set(NIMBLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mynewt-nimble")
file(GLOB WLH_NIMBLE_HOST_SRCS CONFIGURE_DEPENDS "${NIMBLE_DIR}/nimble/host/src/*.c")
# ble_gatts_lcl.c is a debug-only local GATT dump that hard-depends on the
# Mynewt console package; it is not part of the host we ship.
list(REMOVE_ITEM WLH_NIMBLE_HOST_SRCS
    "${NIMBLE_DIR}/nimble/host/src/ble_gatts_lcl.c")
add_library(wlh_nimble STATIC
    ${WLH_NIMBLE_HOST_SRCS}
    "${NIMBLE_DIR}/nimble/host/util/src/addr.c"
    "${NIMBLE_DIR}/nimble/host/services/gap/src/ble_svc_gap.c"
    "${NIMBLE_DIR}/nimble/host/services/gatt/src/ble_svc_gatt.c"
    "${NIMBLE_DIR}/nimble/transport/src/transport.c"
    "${NIMBLE_DIR}/porting/nimble/src/endian.c"
    "${NIMBLE_DIR}/porting/nimble/src/mem.c"
    "${NIMBLE_DIR}/porting/nimble/src/nimble_port.c"
    "${NIMBLE_DIR}/porting/nimble/src/os_mbuf.c"
    "${NIMBLE_DIR}/porting/nimble/src/os_mempool.c"
    "${NIMBLE_DIR}/porting/nimble/src/os_msys.c"
    # Our NimBLE NPL port over wlh::osal (no direct pthread use here).
    src/ble/npl/npl_osal.c)
target_include_directories(wlh_nimble
    PUBLIC
        # WL-hosted overrides MUST precede the upstream porting headers so our
        # syscfg limits and NPL type definitions win the include search.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ble/syscfg"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ble/npl/include"
        "${NIMBLE_DIR}/nimble/include"
        "${NIMBLE_DIR}/nimble/host/include"
        "${NIMBLE_DIR}/nimble/host/util/include"
        "${NIMBLE_DIR}/nimble/host/services/gap/include"
        "${NIMBLE_DIR}/nimble/host/services/gatt/include"
        "${NIMBLE_DIR}/nimble/transport/include"
        "${NIMBLE_DIR}/porting/nimble/include"
    PRIVATE
        "${NIMBLE_DIR}/nimble/host/src")
target_compile_features(wlh_nimble PUBLIC c_std_11)
target_link_libraries(wlh_nimble PUBLIC wlh::osal mbedcrypto)
set_target_properties(wlh_nimble PROPERTIES C_EXTENSIONS OFF)
# Upstream NimBLE does not build clean under -Wall -Werror; keep it quiet.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_nimble PRIVATE -w)
endif()

# Our BLE adapter code (transport bridge, bond store, scenario driver).
add_library(wlh_ble STATIC
    src/ble/hci_transport.c
    src/ble/bond_store.c
    src/ble/ble_app.c)
target_include_directories(wlh_ble PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ble)
target_compile_features(wlh_ble PUBLIC c_std_11)
target_link_libraries(wlh_ble
    PUBLIC wlh_nimble wlh::host_core wlh::log wlh::posix_osal)
set_target_properties(wlh_ble PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_ble PRIVATE -Wall -Werror)
endif()

# --- REPL third-party deps (linenoise + cJSON, vendored submodules) ----------
add_library(wlh_linenoise STATIC third_party/linenoise/linenoise.c)
target_include_directories(wlh_linenoise PUBLIC third_party/linenoise)
target_compile_features(wlh_linenoise PUBLIC c_std_11)
set_target_properties(wlh_linenoise PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_linenoise PRIVATE -w)
endif()

add_library(wlh_cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(wlh_cjson PUBLIC third_party)
target_compile_features(wlh_cjson PUBLIC c_std_11)
set_target_properties(wlh_cjson PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_cjson PRIVATE -w)
endif()

add_executable(wlh-host-macos-sim src/main.c src/repl.c src/repl_json.c)
target_compile_features(wlh-host-macos-sim PRIVATE c_std_11)
target_compile_definitions(wlh-host-macos-sim PRIVATE WLH_ENABLE_TEST_HOOKS)
target_link_libraries(wlh-host-macos-sim PRIVATE
    wlh_host_sim_platform wlh::host_core wlh::posix_osal wlh::log
    wlh_sim_nanopb wlh_ble wlh_linenoise wlh_cjson Threads::Threads)
if(APPLE)
    target_link_libraries(wlh-host-macos-sim PRIVATE "-framework CoreFoundation")
endif()

foreach(target wlh_host_sim_lwip wlh_host_sim_platform wlh-host-macos-sim)
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Werror)
    endif()
endforeach()

if(WLH_SIM_BUILD_TESTING)
    add_executable(wlh_host_sim_ipc_tests tests/test_ipc.c)
    target_link_libraries(wlh_host_sim_ipc_tests PRIVATE wlh_host_sim_platform Threads::Threads)
    target_compile_features(wlh_host_sim_ipc_tests PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_host_sim_ipc_tests PRIVATE
            -Wall -Werror -UNDEBUG)
    elseif(MSVC)
        target_compile_options(wlh_host_sim_ipc_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME wlh_host_sim_ipc COMMAND wlh_host_sim_ipc_tests)

    add_executable(wlh_host_sim_npl_tests tests/test_npl.c)
    target_link_libraries(wlh_host_sim_npl_tests PRIVATE wlh_nimble wlh::posix_osal Threads::Threads)
    target_include_directories(wlh_host_sim_npl_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ble/npl")
    target_compile_features(wlh_host_sim_npl_tests PRIVATE c_std_11)
    set_target_properties(wlh_host_sim_npl_tests PROPERTIES C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_host_sim_npl_tests PRIVATE
            -Wall -Werror -UNDEBUG)
    elseif(MSVC)
        target_compile_options(wlh_host_sim_npl_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME wlh_host_sim_npl COMMAND wlh_host_sim_npl_tests)

    add_executable(wlh_host_sim_repl_json_tests
        tests/test_repl_json.c src/repl_json.c)
    target_include_directories(wlh_host_sim_repl_json_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(wlh_host_sim_repl_json_tests PRIVATE
        wlh_cjson Threads::Threads)
    target_compile_features(wlh_host_sim_repl_json_tests PRIVATE c_std_11)
    set_target_properties(wlh_host_sim_repl_json_tests PROPERTIES
        C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_host_sim_repl_json_tests PRIVATE
            -Wall -Werror -UNDEBUG)
    elseif(MSVC)
        target_compile_options(wlh_host_sim_repl_json_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME wlh_host_sim_repl_json COMMAND wlh_host_sim_repl_json_tests)

endif()
