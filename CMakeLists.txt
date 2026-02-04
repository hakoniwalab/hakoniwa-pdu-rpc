cmake_minimum_required(VERSION 3.16)
project(hakoniwa-pdu-rpc CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
