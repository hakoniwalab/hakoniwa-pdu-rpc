include(FetchContent)

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
  if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()

  # GoogleTest is test infrastructure, not a shipped RPC artifact. Keep it
  # static even if a parent project enables BUILD_SHARED_LIBS globally.
  set(_hako_pdu_rpc_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  FetchContent_MakeAvailable(googletest)
  set(BUILD_SHARED_LIBS "${_hako_pdu_rpc_saved_build_shared_libs}")
  unset(_hako_pdu_rpc_saved_build_shared_libs)
endif()

# Native contract tests retain the fully static RPC boundary that was already
# validated on all supported platforms. The shared C API boundary is exercised
# separately by the Python CFFI integration suite.
set(HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY hakoniwa_pdu_rpc::rpc)

add_executable(hakoniwa_pdu_rpc_basic_test
  rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_basic_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_basic_test COMMAND hakoniwa_pdu_rpc_basic_test)
set_tests_properties(hakoniwa_pdu_rpc_basic_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_mux_server_test
  rpc_mux_server_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_mux_server_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_mux_server_test COMMAND hakoniwa_pdu_rpc_mux_server_test)
set_tests_properties(hakoniwa_pdu_rpc_mux_server_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

# C API parity test for the same two basic contracts. It remains a native
# contract test and therefore links the static RPC target as well.
add_executable(hakoniwa_pdu_rpc_c_api_basic_test
  c_rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_basic_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_basic_test COMMAND hakoniwa_pdu_rpc_c_api_basic_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_basic_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_infinite_wait_test
  rpc_infinite_wait_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_infinite_wait_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_infinite_wait_test COMMAND hakoniwa_pdu_rpc_infinite_wait_test)
set_tests_properties(hakoniwa_pdu_rpc_infinite_wait_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_infinite_wait_test
  c_rpc_infinite_wait_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_infinite_wait_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_infinite_wait_test COMMAND hakoniwa_pdu_rpc_c_api_infinite_wait_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_infinite_wait_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_timeout_cancel_test
  rpc_timeout_cancel_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_timeout_cancel_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_timeout_cancel_test COMMAND hakoniwa_pdu_rpc_timeout_cancel_test)
set_tests_properties(hakoniwa_pdu_rpc_timeout_cancel_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_timeout_cancel_test
  c_rpc_timeout_cancel_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_timeout_cancel_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_timeout_cancel_test COMMAND hakoniwa_pdu_rpc_c_api_timeout_cancel_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_timeout_cancel_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_cancel_race_test
  rpc_cancel_race_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_cancel_race_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_cancel_race_test COMMAND hakoniwa_pdu_rpc_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_cancel_race_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_cancel_race_test
  c_rpc_cancel_race_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_cancel_race_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_cancel_race_test COMMAND hakoniwa_pdu_rpc_c_api_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_cancel_race_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
