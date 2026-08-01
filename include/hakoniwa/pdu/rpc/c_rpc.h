#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HAKO_PDU_RPC_NAME_MAX
#define HAKO_PDU_RPC_NAME_MAX 128
#endif

typedef struct hako_pdu_rpc_client_handle hako_pdu_rpc_client_handle_t;
typedef struct hako_pdu_rpc_server_handle hako_pdu_rpc_server_handle_t;

typedef enum {
    HAKO_PDU_RPC_OK = 0,
    HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT = 1,
    HAKO_PDU_RPC_ERROR_INITIALIZE = 2,
    HAKO_PDU_RPC_ERROR_START = 3,
    HAKO_PDU_RPC_ERROR_NOT_RUNNING = 4,
    HAKO_PDU_RPC_ERROR_CALL = 5,
    HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL = 6,
    HAKO_PDU_RPC_ERROR_NOT_FOUND = 7,
    HAKO_PDU_RPC_ERROR_INTERNAL = 8
} hako_pdu_rpc_error_t;

typedef enum {
    HAKO_PDU_RPC_CLIENT_EVENT_NONE = 0,
    HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN = 1,
    HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL = 2,
    HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT = 3
} hako_pdu_rpc_client_event_t;

typedef enum {
    HAKO_PDU_RPC_SERVER_EVENT_NONE = 0,
    HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN = 1,
    HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL = 2
} hako_pdu_rpc_server_event_t;

typedef struct {
    char service_name[HAKO_PDU_RPC_NAME_MAX];
    size_t pdu_size;
} hako_pdu_rpc_response_info_t;

typedef struct {
    uint64_t request_token;
    char service_name[HAKO_PDU_RPC_NAME_MAX];
    char client_name[HAKO_PDU_RPC_NAME_MAX];
    size_t pdu_size;
} hako_pdu_rpc_request_info_t;

hako_pdu_rpc_client_handle_t* hako_pdu_rpc_client_create(const char* node_id, const char* client_name, const char* service_config_path, const char* endpoint_config_path, uint64_t delta_time_usec, const char* time_source_type);
void hako_pdu_rpc_client_destroy(hako_pdu_rpc_client_handle_t* handle);
hako_pdu_rpc_error_t hako_pdu_rpc_client_start(hako_pdu_rpc_client_handle_t* handle);
hako_pdu_rpc_error_t hako_pdu_rpc_client_stop(hako_pdu_rpc_client_handle_t* handle);
hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer(hako_pdu_rpc_client_handle_t* handle, const char* service_name, uint8_t* buffer, size_t capacity, size_t* out_size);
hako_pdu_rpc_error_t hako_pdu_rpc_client_call(hako_pdu_rpc_client_handle_t* handle, const char* service_name, const uint8_t* pdu, size_t pdu_size, uint64_t timeout_usec);
hako_pdu_rpc_error_t hako_pdu_rpc_client_cancel(hako_pdu_rpc_client_handle_t* handle, const char* service_name);
hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll(hako_pdu_rpc_client_handle_t* handle, hako_pdu_rpc_response_info_t* out_info, uint8_t* buffer, size_t capacity, size_t* out_size, hako_pdu_rpc_error_t* out_error);

hako_pdu_rpc_server_handle_t* hako_pdu_rpc_server_create(const char* node_id, const char* service_config_path, const char* endpoint_config_path, uint64_t delta_time_usec, const char* time_source_type);
void hako_pdu_rpc_server_destroy(hako_pdu_rpc_server_handle_t* handle);
hako_pdu_rpc_error_t hako_pdu_rpc_server_start(hako_pdu_rpc_server_handle_t* handle);
hako_pdu_rpc_error_t hako_pdu_rpc_server_stop(hako_pdu_rpc_server_handle_t* handle);
hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll(hako_pdu_rpc_server_handle_t* handle, hako_pdu_rpc_request_info_t* out_info, uint8_t* buffer, size_t capacity, size_t* out_size, hako_pdu_rpc_error_t* out_error);
hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer(hako_pdu_rpc_server_handle_t* handle, uint64_t request_token, uint8_t status, int32_t result_code, uint8_t* buffer, size_t capacity, size_t* out_size);
hako_pdu_rpc_error_t hako_pdu_rpc_server_send_reply(hako_pdu_rpc_server_handle_t* handle, uint64_t request_token, const uint8_t* pdu, size_t pdu_size);
hako_pdu_rpc_error_t hako_pdu_rpc_server_send_cancel_reply(hako_pdu_rpc_server_handle_t* handle, uint64_t request_token, const uint8_t* pdu, size_t pdu_size);

/* Co-located test/application shutdown order: server Endpoint, client Endpoint, server RPC, client RPC. */
hako_pdu_rpc_error_t hako_pdu_rpc_stop_pair(hako_pdu_rpc_server_handle_t* server, hako_pdu_rpc_client_handle_t* client);

#ifdef __cplusplus
}
#endif
