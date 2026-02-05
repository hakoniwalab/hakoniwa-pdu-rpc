include(FetchContent)

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

add_library(hakoniwa_pdu_rpc
    rpc_server_endpoint_impl.cpp
    rpc_services_server.cpp
    rpc_client_endpoint_impl.cpp
    rpc_services_client.cpp
)

add_library(hakoniwa_pdu_rpc::hakoniwa_pdu_rpc ALIAS hakoniwa_pdu_rpc)

target_include_directories(hakoniwa_pdu_rpc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types>
    $<BUILD_INTERFACE:/usr/local/hakoniwa/include>
    $<BUILD_INTERFACE:/usr/local/hakoniwa/include/hakoniwa>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(hakoniwa_pdu_endpoint REQUIRED)

# Link our library against the endpoint library (installed)
target_link_libraries(hakoniwa_pdu_rpc
  PUBLIC
    hakoniwa_pdu_endpoint
    nlohmann_json::nlohmann_json
)

set_target_properties(hakoniwa_pdu_rpc PROPERTIES EXPORT_NAME hakoniwa_pdu_rpc)

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
