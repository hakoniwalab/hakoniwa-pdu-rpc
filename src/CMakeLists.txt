add_library(hakoniwa_pdu_rpc
    pdu_rpc_server_endpoint_iml.cpp
    rpc_services_server.cpp
    #rpc_services_client.cpp
)

target_include_directories(hakoniwa_pdu_rpc
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${HAKONIWA_ROS2PDU_DIR}/pdu/types
)

# Add the subdirectory for the endpoint, which will define the hakoniwa_pdu_endpoint target
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../hakoniwa-pdu-endpoint ${CMAKE_BINARY_DIR}/hakoniwa-pdu-endpoint)

# Link our library against the endpoint library
target_link_libraries(hakoniwa_pdu_rpc
  PUBLIC
    hakoniwa_pdu_endpoint
)
