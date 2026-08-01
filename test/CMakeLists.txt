include(FetchContent)

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
  if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()

  # GoogleTest is a build-only test dependency. Keep its install rules and
  # unused GoogleMock targets out of the product install surface so that a
  # test-enabled build tree remains safe to pass to `cmake --install`.
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  FetchContent_MakeAvailable(googletest)
endif()

# The C++ contract tests intentionally link to the internal test runtime on
# Windows shared builds. This keeps EndpointContainer, RpcServicesClient, and
# RpcServicesServer in the same module and avoids crossing the DLL boundary
# with C++/STL-owned objects. On other configurations the alias resolves to the
# normal product target.
set(HAKO_PDU_RPC_CPP_TEST_TARGET hakoniwa_pdu_rpc::test_runtime)

# Phase 1: focused, cross-platform-safe RPC contract tests.
add_executable(hakoniwa_pdu_rpc_basic_test
  rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_basic_test
  PRIVATE
    ${HAKO_PDU_RPC_CPP_TEST_TARGET}
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_basic_test COMMAND hakoniwa_pdu_rpc_basic_test)
set_tests_properties(hakoniwa_pdu_rpc_basic_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

# Phase 2: timeout == 0 means the in-flight RPC has no timeout deadline.
add_executable(hakoniwa_pdu_rpc_infinite_wait_test
  rpc_infinite_wait_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_infinite_wait_test
  PRIVATE
    ${HAKO_PDU_RPC_CPP_TEST_TARGET}
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_infinite_wait_test COMMAND hakoniwa_pdu_rpc_infinite_wait_test)
set_tests_properties(hakoniwa_pdu_rpc_infinite_wait_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

# Phase 3: timeout is a notification; cancellation is explicit; only a terminal
# cancel response returns the endpoint to a reusable state.
add_executable(hakoniwa_pdu_rpc_timeout_cancel_test
  rpc_timeout_cancel_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_timeout_cancel_test
  PRIVATE
    ${HAKO_PDU_RPC_CPP_TEST_TARGET}
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_timeout_cancel_test COMMAND hakoniwa_pdu_rpc_timeout_cancel_test)
set_tests_properties(hakoniwa_pdu_rpc_timeout_cancel_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

# Phase 4: after explicit cancellation, a normal response may still win before
# the server processes the CANCEL. A late CANCEL must be harmless.
add_executable(hakoniwa_pdu_rpc_cancel_race_test
  rpc_cancel_race_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_cancel_race_test
  PRIVATE
    ${HAKO_PDU_RPC_CPP_TEST_TARGET}
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_cancel_race_test COMMAND hakoniwa_pdu_rpc_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_cancel_race_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

# C ABI smoke coverage must link to the distributed product target. This verifies
# that the C API is exported from hakoniwa_pdu_rpc.dll/.so/.dylib without exposing
# C++ ownership across the language boundary.
add_executable(hakoniwa_pdu_rpc_c_api_test
  c_rpc_api_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_test
  PRIVATE
    hakoniwa_pdu_rpc::rpc
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_c_api_test COMMAND hakoniwa_pdu_rpc_c_api_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)
