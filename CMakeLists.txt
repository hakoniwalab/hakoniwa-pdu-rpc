cmake_minimum_required(VERSION 3.16)
project(hakoniwa-pdu-rpc VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Default install prefix for this project
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr/local/hakoniwa" CACHE PATH "Install prefix" FORCE)
endif()

# Default install prefix for hakoniwa-pdu-endpoint (compatible with hakoniwa-pdu-bridge)
set(HAKO_PDU_ENDPOINT_PREFIX "/usr/local/hakoniwa" CACHE PATH "Install prefix for hakoniwa-pdu-endpoint")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Option to build tests, ON by default
option(HAKO_PDU_RPC_BUILD_TESTS "Build tests for hakoniwa-pdu-rpc" ON)
option(HAKO_PDU_RPC_BUILD_EXAMPLES "Build examples for hakoniwa-pdu-rpc" OFF)

set(HAKONIWA_PDU_REGISTRY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hakoniwa-pdu-registry")
message(STATUS "HAKONIWA_PDU_REGISTRY_DIR: ${HAKONIWA_PDU_REGISTRY_DIR}"
)
# Add src subdirectory
add_subdirectory(src)

# Enable and add test subdirectory if the option is ON
if(HAKO_PDU_RPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(HAKO_PDU_RPC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

set(HAKO_PDU_RPC_INSTALL_CMAKEDIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/hakoniwa_pdu_rpc"
  CACHE PATH "CMake package config directory for hakoniwa_pdu_rpc"
)

install(
  EXPORT hakoniwa_pdu_rpcTargets
  FILE hakoniwa_pdu_rpcTargets.cmake
  NAMESPACE hakoniwa_pdu_rpc::
  DESTINATION "${HAKO_PDU_RPC_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hakoniwa_pdu_rpcConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/hakoniwa_pdu_rpcConfig.cmake"
  INSTALL_DESTINATION "${HAKO_PDU_RPC_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/hakoniwa_pdu_rpcConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/hakoniwa_pdu_rpcConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/hakoniwa_pdu_rpcConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindHakoniwaPduEndpoint.cmake"
  DESTINATION "${HAKO_PDU_RPC_INSTALL_CMAKEDIR}"
)
