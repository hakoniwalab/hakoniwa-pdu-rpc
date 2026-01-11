find_package(GTest REQUIRED)

add_executable(hakoniwa_pdu_rpc_test
    hakoniwa_pdu_rpc_test.cpp
    #abnormal_case_test.cpp
)
if(APPLE)
  set(HAKONIWA_TEST_LIB_DIRS /usr/local/hakoniwa/lib)
else()
  set(HAKONIWA_TEST_LIB_DIRS /usr/local/hakoniwa/lib /usr/lib/x86_64-linux-gnu)
endif()
target_link_directories(hakoniwa_pdu_rpc_test PRIVATE ${HAKONIWA_TEST_LIB_DIRS})

set(HAKONIWA_TEST_LINK_LIBS "")
foreach(lib_name shakoc hako assets conductor)
  find_library(HAKONIWA_TEST_${lib_name}_LIB NAMES ${lib_name} PATHS ${HAKONIWA_TEST_LIB_DIRS})
  if(HAKONIWA_TEST_${lib_name}_LIB)
    list(APPEND HAKONIWA_TEST_LINK_LIBS ${HAKONIWA_TEST_${lib_name}_LIB})
  else()
    list(APPEND HAKONIWA_TEST_LINK_LIBS ${lib_name})
  endif()
endforeach()

target_link_libraries(hakoniwa_pdu_rpc_test
  PRIVATE
    hakoniwa_pdu_rpc
    ${HAKONIWA_TEST_LINK_LIBS}
    GTest::gtest_main
)

add_test(NAME hakoniwa_pdu_rpc_test COMMAND hakoniwa_pdu_rpc_test)
