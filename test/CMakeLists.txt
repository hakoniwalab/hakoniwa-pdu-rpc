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

add_executable(hakoniwa_pdu_rpc_test
  hakoniwa_pdu_rpc_test.cpp
  rpc_timeout_cancel_semantics_test.cpp
)

target_link_libraries(hakoniwa_pdu_rpc_test
  PRIVATE
    hakoniwa_pdu_rpc::rpc
    GTest::gtest_main
)

# The legacy test file still includes <unistd.h>. Keep the production library
# fully portable and isolate the small MSVC compatibility shim to this test only.
if(WIN32)
  target_include_directories(hakoniwa_pdu_rpc_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  )
endif()

add_test(NAME hakoniwa_pdu_rpc_test COMMAND hakoniwa_pdu_rpc_test)
set_tests_properties(hakoniwa_pdu_rpc_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  TIMEOUT 30
)
