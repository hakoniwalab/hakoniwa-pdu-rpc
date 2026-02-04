include(FetchContent)

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

target_include_directories(hakoniwa_pdu_rpc
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${HAKONIWA_PDU_REGISTRY_DIR}/pdu/types
    /usr/local/hakoniwa/include/hakoniwa
)

find_package(HakoniwaPduEndpoint REQUIRED)

# Link our library against the endpoint library (installed)
target_link_libraries(hakoniwa_pdu_rpc
  PUBLIC
    hakoniwa_pdu_endpoint
    nlohmann_json::nlohmann_json
)
