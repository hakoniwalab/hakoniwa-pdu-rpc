cmake_minimum_required(VERSION 3.10)
project(hakoniwa-pdu-rpc CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to build tests, ON by default
option(HAKO_PDU_RPC_BUILD_TESTS "Build tests for hakoniwa-pdu-rpc" ON)

# Add src subdirectory
add_subdirectory(src)

# Enable and add test subdirectory if the option is ON
if(HAKO_PDU_RPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
