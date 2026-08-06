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

function(hako_pdu_rpc_stage_windows_test_dlls target)
  if(NOT WIN32)
    return()
  endif()

  # CTest does not inherit the DLL discovery performed by the Python package.
  # Stage the runtime DLLs beside each test executable so Windows follows its
  # normal application-directory lookup without modifying PATH globally.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:hakoniwa_pdu_rpc_shared>
      $<TARGET_FILE_DIR:${target}>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint>
      $<TARGET_FILE_DIR:${target}>
    VERBATIM
  )
endfunction()

add_executable(hakoniwa_pdu_action_configuration_test
  action_configuration_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_action_configuration_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
target_include_directories(hakoniwa_pdu_action_configuration_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(hakoniwa_pdu_action_configuration_test PRIVATE
  ACTION_CONFIG_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/sample/action.json"
)
add_test(NAME hakoniwa_pdu_action_configuration_test COMMAND hakoniwa_pdu_action_configuration_test)
set_tests_properties(hakoniwa_pdu_action_configuration_test PROPERTIES TIMEOUT 30)

add_executable(hakoniwa_pdu_action_server_initialization_test
  action_server_initialization_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_action_server_initialization_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
target_include_directories(hakoniwa_pdu_action_server_initialization_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(hakoniwa_pdu_action_server_initialization_test PRIVATE
  ACTION_SERVER_ENDPOINT_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/configs/action_server_endpoint.json"
)
add_test(NAME hakoniwa_pdu_action_server_initialization_test COMMAND hakoniwa_pdu_action_server_initialization_test)
set_tests_properties(hakoniwa_pdu_action_server_initialization_test PROPERTIES TIMEOUT 30)

add_executable(hakoniwa_pdu_action_packet_codec_test
  action_packet_codec_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_action_packet_codec_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_action_packet_codec_test COMMAND hakoniwa_pdu_action_packet_codec_test)
set_tests_properties(hakoniwa_pdu_action_packet_codec_test PROPERTIES TIMEOUT 30)

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

add_executable(hakoniwa_pdu_rpc_c_api_mux_server_test
  c_rpc_mux_server_contract_test.cpp
)
target_link_libraries(hakoniwa_pdu_rpc_c_api_mux_server_test PRIVATE ${HAKO_PDU_RPC_NATIVE_CONTRACT_LIBRARY} GTest::gtest_main)
add_test(NAME hakoniwa_pdu_rpc_c_api_mux_server_test COMMAND hakoniwa_pdu_rpc_c_api_mux_server_test)
set_tests_properties(hakoniwa_pdu_rpc_c_api_mux_server_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

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

set(HAKO_PDU_RPC_NATIVE_CONTRACT_TARGETS
  hakoniwa_pdu_action_configuration_test
  hakoniwa_pdu_action_server_initialization_test
  hakoniwa_pdu_action_packet_codec_test
  hakoniwa_pdu_rpc_basic_test
  hakoniwa_pdu_rpc_mux_server_test
  hakoniwa_pdu_rpc_c_api_mux_server_test
  hakoniwa_pdu_rpc_c_api_basic_test
  hakoniwa_pdu_rpc_infinite_wait_test
  hakoniwa_pdu_rpc_c_api_infinite_wait_test
  hakoniwa_pdu_rpc_timeout_cancel_test
  hakoniwa_pdu_rpc_c_api_timeout_cancel_test
  hakoniwa_pdu_rpc_cancel_race_test
  hakoniwa_pdu_rpc_c_api_cancel_race_test
)

foreach(target IN LISTS HAKO_PDU_RPC_NATIVE_CONTRACT_TARGETS)
  hako_pdu_rpc_stage_windows_test_dlls(${target})
endforeach()
