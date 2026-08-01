include(FetchContent)

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
  if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(hakoniwa_pdu_rpc_basic_test
  rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_basic_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_basic_test COMMAND hakoniwa_pdu_rpc_basic_test)
set_tests_properties(hakoniwa_pdu_rpc_basic_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

# C API parity test for the same two basic contracts. Keep this beside the
# existing C++ test until the C facade has proven equivalent on every platform.
add_executable(hakoniwa_pdu_rpc_c_api_basic_test
  c_rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_basic_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_basic_test COMMAND hakoniwa_pdu_rpc_c_api_basic_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_basic_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_infinite_wait_test
  rpc_infinite_wait_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_infinite_wait_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_infinite_wait_test COMMAND hakoniwa_pdu_rpc_infinite_wait_test)
set_tests_properties(hakoniwa_pdu_rpc_infinite_wait_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_infinite_wait_test
  c_rpc_infinite_wait_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_infinite_wait_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_infinite_wait_test COMMAND hakoniwa_pdu_rpc_c_api_infinite_wait_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_infinite_wait_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_timeout_cancel_test
  rpc_timeout_cancel_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_timeout_cancel_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_timeout_cancel_test COMMAND hakoniwa_pdu_rpc_timeout_cancel_test)
set_tests_properties(hakoniwa_pdu_rpc_timeout_cancel_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_timeout_cancel_test
  c_rpc_timeout_cancel_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_timeout_cancel_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_timeout_cancel_test COMMAND hakoniwa_pdu_rpc_c_api_timeout_cancel_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_timeout_cancel_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_cancel_race_test
  rpc_cancel_race_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_cancel_race_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_cancel_race_test COMMAND hakoniwa_pdu_rpc_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_cancel_race_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

add_executable(hakoniwa_pdu_rpc_c_api_cancel_race_test
  c_rpc_cancel_race_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_cancel_race_test PRIVATE hakoniwa_pdu_rpc::rpc GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_cancel_race_test COMMAND hakoniwa_pdu_rpc_c_api_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_cancel_race_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
