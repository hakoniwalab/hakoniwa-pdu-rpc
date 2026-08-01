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

  # The Python distribution profile uses BUILD_SHARED_LIBS=ON for the C facade,
  # but the existing C++ contract tests should retain their original static
  # GoogleTest dependency.
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

# Existing C++ contract tests continue to exercise the original C++ library.
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

# C ABI coverage links to the C facade. In the Python distribution profile this
# is the actual DLL/.so/.dylib loaded by CFFI.
add_executable(hakoniwa_pdu_rpc_c_api_test
  c_rpc_api_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_test
  PRIVATE
    hakoniwa_pdu_rpc::c
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_c_api_test COMMAND hakoniwa_pdu_rpc_c_api_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)

add_executable(hakoniwa_pdu_rpc_c_roundtrip_test
  c_rpc_roundtrip_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_roundtrip_test
  PRIVATE
    hakoniwa_pdu_rpc::c
    GTest::gtest_main
)
add_test(NAME hakoniwa_pdu_rpc_c_roundtrip_test COMMAND hakoniwa_pdu_rpc_c_roundtrip_test)
set_tests_properties(hakoniwa_pdu_rpc_c_roundtrip_test PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  TIMEOUT 30
)
