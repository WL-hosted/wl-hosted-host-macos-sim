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
    target_compile_options(wlh_easylogger PRIVATE -Wall -Wextra -Werror)
endif()

# The core log backend needs EasyLogger headers whenever a translation unit
# includes wlh/log.h with the EASYLOGGER backend selected.
target_link_libraries(wlh_log_backend PUBLIC wlh::easylogger)

add_library(wlh_host_sim_platform STATIC
    src/ipc.c src/executor.c src/transport_usb.c)
target_include_directories(wlh_host_sim_platform
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(wlh_host_sim_platform PUBLIC c_std_11)
target_link_libraries(wlh_host_sim_platform
    PUBLIC Threads::Threads PkgConfig::LIBUSB wlh::log)

add_executable(wlh-host-macos-sim src/main.c)
target_compile_features(wlh-host-macos-sim PRIVATE c_std_11)
target_compile_definitions(wlh-host-macos-sim PRIVATE WLH_ENABLE_TEST_HOOKS)
target_link_libraries(wlh-host-macos-sim PRIVATE
    wlh_host_sim_platform wlh::host_core wlh::posix_osal wlh::log
    wlh::easylogger wlh_sim_nanopb Threads::Threads)

foreach(target wlh_host_sim_platform wlh-host-macos-sim)
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

if(WLH_SIM_BUILD_TESTING)
    add_executable(wlh_host_sim_ipc_tests tests/test_ipc.c)
    target_link_libraries(wlh_host_sim_ipc_tests PRIVATE wlh_host_sim_platform Threads::Threads)
    target_compile_features(wlh_host_sim_ipc_tests PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_host_sim_ipc_tests PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    elseif(MSVC)
        target_compile_options(wlh_host_sim_ipc_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME wlh_host_sim_ipc COMMAND wlh_host_sim_ipc_tests)

endif()
