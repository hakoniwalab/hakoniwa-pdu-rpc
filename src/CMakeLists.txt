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

set(HAKO_PDU_RPC_SOURCES
  rpc_server_endpoint_impl.cpp
  rpc_services_server.cpp
  rpc_client_endpoint_impl.cpp
  rpc_services_client.cpp
  c_rpc.cpp
  c_rpc_cancel.cpp
  c_rpc_alloc.cpp
)

function(hako_pdu_rpc_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  target_link_libraries(${target}
    PUBLIC
      hakoniwa_pdu_endpoint::hakoniwa_pdu_endpoint
      nlohmann_json::nlohmann_json
  )
endfunction()

# Keep the historical native CMake target static. Existing C++ consumers and
# all native contract tests therefore retain the same linkage that was already
# validated across Linux, macOS, and Windows.
add_library(hakoniwa_pdu_rpc STATIC ${HAKO_PDU_RPC_SOURCES})
hako_pdu_rpc_configure_target(hakoniwa_pdu_rpc)
set_target_properties(hakoniwa_pdu_rpc PROPERTIES EXPORT_NAME rpc)

add_library(hakoniwa_pdu_rpc::rpc ALIAS hakoniwa_pdu_rpc)
add_library(hakoniwa_pdu_rpc::hakoniwa_pdu_rpc ALIAS hakoniwa_pdu_rpc)

# Build the CFFI-loadable artifact beside the static library from the same
# sources. The runtime file keeps the established hakoniwa_pdu_rpc name, while
# its Windows import library uses a distinct name to avoid colliding with the
# real static hakoniwa_pdu_rpc.lib.
add_library(hakoniwa_pdu_rpc_shared SHARED ${HAKO_PDU_RPC_SOURCES})
hako_pdu_rpc_configure_target(hakoniwa_pdu_rpc_shared)
set_target_properties(hakoniwa_pdu_rpc_shared PROPERTIES
  EXPORT_NAME rpc_shared
  OUTPUT_NAME hakoniwa_pdu_rpc
  ARCHIVE_OUTPUT_NAME hakoniwa_pdu_rpc_shared
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

add_library(hakoniwa_pdu_rpc::rpc_shared ALIAS hakoniwa_pdu_rpc_shared)

install(
  TARGETS
    hakoniwa_pdu_rpc
    hakoniwa_pdu_rpc_shared
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
