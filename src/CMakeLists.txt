include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
  option(HAKO_PDU_RPC_INSTALL_NLOHMANN_JSON "Install bundled nlohmann_json when installing hakoniwa-pdu-rpc" ON)
  if(HAKO_PDU_RPC_INSTALL_NLOHMANN_JSON)
    set(JSON_Install ON CACHE INTERNAL "")
  endif()

  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(HAKONIWA_PDU_RPC_SOURCES
  rpc_server_endpoint_impl.cpp
  rpc_services_server.cpp
  rpc_client_endpoint_impl.cpp
  rpc_services_client.cpp
  c_rpc.cpp
)

function(hakoniwa_pdu_rpc_configure_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_include_directories(${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  target_link_libraries(${target_name}
    PUBLIC
      hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
      nlohmann_json::nlohmann_json
  )
endfunction()

add_library(hakoniwa_pdu_rpc ${HAKONIWA_PDU_RPC_SOURCES})
hakoniwa_pdu_rpc_configure_target(hakoniwa_pdu_rpc)

add_library(hakoniwa_pdu_rpc::rpc ALIAS hakoniwa_pdu_rpc)
add_library(hakoniwa_pdu_rpc::hakoniwa_pdu_rpc ALIAS hakoniwa_pdu_rpc)

set_target_properties(hakoniwa_pdu_rpc PROPERTIES EXPORT_NAME rpc)

# A Windows DLL needs exported symbols before CMake/MSVC can produce the
# corresponding import library. The CFFI boundary consumes the C API from this
# target; existing C++ contract tests use an internal static target below so
# C++ objects and EndpointContainer instances never cross the DLL boundary.
if(WIN32 AND BUILD_SHARED_LIBS)
  set_target_properties(hakoniwa_pdu_rpc PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
  )
endif()

# The existing contract tests exercise the C++ API directly and construct
# EndpointContainer objects in the test process. On Windows, linking those tests
# to the shared RPC target would pass C++/STL-owned objects across a DLL boundary
# while Endpoint is statically embedded in the DLL. Build the same implementation
# as an internal static test runtime instead. The distributed C ABI still uses
# hakoniwa_pdu_rpc.dll and is covered by the C API test and Python smoke test.
if(WIN32 AND BUILD_SHARED_LIBS AND HAKO_PDU_RPC_BUILD_TESTS)
  add_library(hakoniwa_pdu_rpc_test_runtime STATIC ${HAKONIWA_PDU_RPC_SOURCES})
  hakoniwa_pdu_rpc_configure_target(hakoniwa_pdu_rpc_test_runtime)
  add_library(hakoniwa_pdu_rpc::test_runtime ALIAS hakoniwa_pdu_rpc_test_runtime)
else()
  add_library(hakoniwa_pdu_rpc::test_runtime ALIAS hakoniwa_pdu_rpc)
endif()

install(
  TARGETS hakoniwa_pdu_rpc
  EXPORT hakoniwa_pdu_rpcTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY "${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
)
