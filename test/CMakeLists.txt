find_package(GTest REQUIRED)

add_executable(hakoniwa_pdu_rpc_test
    hakoniwa_pdu_rpc_test.cpp
    #abnormal_case_test.cpp
)
target_link_directories(hakoniwa_pdu_rpc_test PRIVATE /usr/local/hakoniwa/lib)

target_link_libraries(hakoniwa_pdu_rpc_test
  PRIVATE
    hakoniwa_pdu_rpc
    shakoc
    GTest::gtest_main
)

add_test(NAME hakoniwa_pdu_rpc_test COMMAND hakoniwa_pdu_rpc_test)
