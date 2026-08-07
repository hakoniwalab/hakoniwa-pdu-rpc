cmake_minimum_required(VERSION 3.16)
project(hakoniwa-pdu-rpc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Preserve the historical Unix default while allowing native CMake defaults on
# Windows. tools/hako.py always uses an explicit local install prefix.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT WIN32)
  set(CMAKE_INSTALL_PREFIX "/usr/local/hakoniwa" CACHE PATH "Install prefix" FORCE)
endif()

# Prefer the installed Endpoint CMake package. A compatibility Find module is
# retained for older Endpoint installations, but the public dependency target
# is always normalized to hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint.
set(HAKO_PDU_ENDPOINT_PREFIX "" CACHE PATH "Optional install prefix for hakoniwa-pdu-endpoint")
if(HAKO_PDU_ENDPOINT_PREFIX STREQUAL "" AND DEFINED ENV{HAKO_PDU_ENDPOINT_PREFIX})
  set(HAKO_PDU_ENDPOINT_PREFIX "$ENV{HAKO_PDU_ENDPOINT_PREFIX}" CACHE PATH
      "Optional install prefix for hakoniwa-pdu-endpoint" FORCE)
elseif(HAKO_PDU_ENDPOINT_PREFIX STREQUAL "" AND DEFINED ENV{HAKO_PDU_ENDPOINT_ROOT})
  set(HAKO_PDU_ENDPOINT_PREFIX "$ENV{HAKO_PDU_ENDPOINT_ROOT}" CACHE PATH
      "Optional install prefix for hakoniwa-pdu-endpoint" FORCE)
endif()

set(_hako_endpoint_hints "")
if(NOT HAKO_PDU_ENDPOINT_PREFIX STREQUAL "")
  list(APPEND _hako_endpoint_hints
    "${HAKO_PDU_ENDPOINT_PREFIX}"
    "${HAKO_PDU_ENDPOINT_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/hakoniwa_pdu_endpoint"
  )
endif()

find_package(hakoniwa_pdu_endpoint CONFIG QUIET HINTS ${_hako_endpoint_hints})
if(NOT TARGET hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(HakoniwaPduEndpoint REQUIRED)
endif()
if(NOT TARGET hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint)
  message(FATAL_ERROR
    "hakoniwa-pdu-endpoint dependency did not provide "
    "hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint")
endif()

option(HAKO_PDU_RPC_BUILD_TESTS "Build tests for hakoniwa-pdu-rpc" ON)
option(HAKO_PDU_RPC_BUILD_EXAMPLES "Build examples for hakoniwa-pdu-rpc" OFF)

set(HAKONIWA_PDU_REGISTRY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hakoniwa-pdu-registry" CACHE PATH
    "Source checkout of hakoniwa-pdu-registry used for generated public PDU headers")
if(NOT EXISTS "${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types")
  message(FATAL_ERROR
    "hakoniwa-pdu-registry generated types were not found at "
    "${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types. Run: git submodule update --init --recursive")
endif()

add_subdirectory(src)

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
