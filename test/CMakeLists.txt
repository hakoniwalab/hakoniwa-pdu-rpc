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

# Phase 1: focused, cross-platform-safe RPC contract tests.
add_executable(hakoniwa_pdu_rpc_basic_test
  rpc_basic_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_basic_test
  PRIVATE
    hakoniwa_pdu_rpc::rpc
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
    hakoniwa_pdu_rpc::rpc
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
    hakoniwa_pdu_rpc::rpc
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
    hakoniwa_pdu_rpc::rpc
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_cancel_race_test COMMAND hakoniwa_pdu_rpc_cancel_race_test)
set_tests_properties(hakoniwa_pdu_rpc_cancel_race_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

# Legacy aggregate tests are intentionally preserved unchanged for audit.
# They are not part of the phased cross-platform gates.
add_executable(hakoniwa_pdu_rpc_test
    hakoniwa_pdu_rpc_test.cpp
    rpc_timeout_cancel_semantics_test.cpp
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
