find_package(GTest REQUIRED)

add_executable(hakoniwa_pdu_rpc_test
    hakoniwa_pdu_rpc_test.cpp
    abnormal_case_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_test
  PRIVATE
    hakoniwa_pdu_rpc
    GTest::gtest_main
)

add_test(NAME hakoniwa_pdu_rpc_test COMMAND hakoniwa_pdu_rpc_test)
