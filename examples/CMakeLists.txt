add_executable(hakoniwa_pdu_rpc_server rpc_server.cpp)
add_executable(hakoniwa_pdu_rpc_client rpc_client.cpp)

target_link_libraries(hakoniwa_pdu_rpc_server
  PRIVATE
    hakoniwa_pdu_rpc
)

target_link_libraries(hakoniwa_pdu_rpc_client
  PRIVATE
    hakoniwa_pdu_rpc
)
