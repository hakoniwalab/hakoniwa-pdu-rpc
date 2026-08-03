#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HAKO_PDU_ACTION_NAME_MAX
#define HAKO_PDU_ACTION_NAME_MAX 128
#endif

#define HAKO_PDU_ACTION_GOAL_ID_SIZE 16

typedef struct hako_pdu_action_client_handle hako_pdu_action_client_handle_t;
typedef struct hako_pdu_action_server_handle hako_pdu_action_server_handle_t;

typedef uint64_t hako_pdu_action_event_token_t;
typedef uint64_t hako_pdu_action_goal_token_t;

typedef struct {
    uint8_t bytes[HAKO_PDU_ACTION_GOAL_ID_SIZE];
} hako_pdu_action_goal_id_t;

/* High-level bindings should wrap this value as an ActionGoalHandle object. */
typedef struct {
    hako_pdu_action_goal_id_t goal_id;
} hako_pdu_action_client_goal_handle_t;

typedef enum {
    HAKO_PDU_ACTION_OK = 0,
    HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT = 1,
    HAKO_PDU_ACTION_ERROR_INITIALIZE = 2,
    HAKO_PDU_ACTION_ERROR_START = 3,
    HAKO_PDU_ACTION_ERROR_NOT_RUNNING = 4,
    HAKO_PDU_ACTION_ERROR_SEND = 5,
    HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL = 6,
    HAKO_PDU_ACTION_ERROR_NOT_FOUND = 7,
    HAKO_PDU_ACTION_ERROR_INVALID_STATE = 8,
    HAKO_PDU_ACTION_ERROR_INTERNAL = 9
} hako_pdu_action_error_t;

typedef enum {
    HAKO_PDU_ACTION_CLIENT_EVENT_NONE = 0,
    HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE = 1,
    HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK = 2,
    HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE = 3,
    HAKO_PDU_ACTION_CLIENT_EVENT_RESULT = 4,
    HAKO_PDU_ACTION_CLIENT_EVENT_TIMEOUT = 5,
    HAKO_PDU_ACTION_CLIENT_EVENT_ERROR = 6
} hako_pdu_action_client_event_t;

typedef enum {
    HAKO_PDU_ACTION_SERVER_EVENT_NONE = 0,
    HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST = 1,
    HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST = 2,
    HAKO_PDU_ACTION_SERVER_EVENT_RUNTIME_CANCEL_REQUEST = 3,
    HAKO_PDU_ACTION_SERVER_EVENT_ERROR = 4
} hako_pdu_action_server_event_t;

typedef enum {
    HAKO_PDU_ACTION_DECISION_UNSPECIFIED = 0,
    HAKO_PDU_ACTION_DECISION_ACCEPTED = 1,
    HAKO_PDU_ACTION_DECISION_REJECTED = 2
} hako_pdu_action_decision_t;

typedef enum {
    HAKO_PDU_ACTION_TERMINAL_UNSPECIFIED = 0,
    HAKO_PDU_ACTION_TERMINAL_SUCCEEDED = 1,
    HAKO_PDU_ACTION_TERMINAL_CANCELED = 2,
    HAKO_PDU_ACTION_TERMINAL_ABORTED = 3
} hako_pdu_action_terminal_status_t;

typedef enum {
    HAKO_PDU_ACTION_RUNTIME_CANCEL_UNSPECIFIED = 0,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_TRANSPORT_DISCONNECTED = 1,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_SERVER_SHUTDOWN = 2,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_INTERNAL_POLICY = 3
} hako_pdu_action_runtime_cancel_cause_t;

typedef struct {
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_client_goal_handle_t goal;
    hako_pdu_action_decision_t decision;
    hako_pdu_action_terminal_status_t terminal_status;
    uint32_t feedback_sequence;
    size_t pdu_size;
} hako_pdu_action_client_event_info_t;

typedef struct {
    hako_pdu_action_event_token_t event_token;
    hako_pdu_action_goal_token_t goal_token;
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    char client_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_goal_id_t goal_id;
    hako_pdu_action_runtime_cancel_cause_t runtime_cancel_cause;
    size_t pdu_size;
} hako_pdu_action_server_event_info_t;

/* Caller owns buffers returned by *_alloc and releases them with this function. */
void hako_pdu_action_buffer_free(uint8_t* buffer);

hako_pdu_action_client_handle_t* hako_pdu_action_client_create(
    const char* node_id,
    const char* client_name,
    const char* action_config_path,
    const char* endpoint_config_path,
    uint64_t delta_time_usec,
    const char* time_source_type);
void hako_pdu_action_client_destroy(hako_pdu_action_client_handle_t* handle);
hako_pdu_action_error_t hako_pdu_action_client_start(hako_pdu_action_client_handle_t* handle);
hako_pdu_action_error_t hako_pdu_action_client_stop(hako_pdu_action_client_handle_t* handle);
hako_pdu_action_error_t hako_pdu_action_client_create_goal_buffer(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size);
hako_pdu_action_error_t hako_pdu_action_client_create_goal_buffer_alloc(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    uint8_t** out_buffer,
    size_t* out_size);

/*
 * requested_goal_id may be NULL. In that case the Runtime generates a UUID.
 * out_goal receives the actual Goal identity and is used for cancel requests.
 */
hako_pdu_action_error_t hako_pdu_action_client_send_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const uint8_t* pdu,
    size_t pdu_size,
    const hako_pdu_action_goal_id_t* requested_goal_id,
    hako_pdu_action_client_goal_handle_t* out_goal,
    uint64_t timeout_usec);
hako_pdu_action_error_t hako_pdu_action_client_cancel(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_client_goal_handle_t* goal);
hako_pdu_action_client_event_t hako_pdu_action_client_poll(
    hako_pdu_action_client_handle_t* handle,
    hako_pdu_action_client_event_info_t* out_info,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size,
    hako_pdu_action_error_t* out_error);
hako_pdu_action_client_event_t hako_pdu_action_client_poll_alloc(
    hako_pdu_action_client_handle_t* handle,
    hako_pdu_action_client_event_info_t* out_info,
    uint8_t** out_buffer,
    size_t* out_size,
    hako_pdu_action_error_t* out_error);

hako_pdu_action_server_handle_t* hako_pdu_action_server_create(
    const char* node_id,
    const char* action_config_path,
    const char* endpoint_config_path,
    uint64_t delta_time_usec,
    const char* time_source_type);
void hako_pdu_action_server_destroy(hako_pdu_action_server_handle_t* handle);
hako_pdu_action_error_t hako_pdu_action_server_start(hako_pdu_action_server_handle_t* handle);
hako_pdu_action_error_t hako_pdu_action_server_stop(hako_pdu_action_server_handle_t* handle);
hako_pdu_action_server_event_t hako_pdu_action_server_poll(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size,
    hako_pdu_action_error_t* out_error);
hako_pdu_action_server_event_t hako_pdu_action_server_poll_alloc(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    uint8_t** out_buffer,
    size_t* out_size,
    hako_pdu_action_error_t* out_error);

hako_pdu_action_error_t hako_pdu_action_server_accept_goal(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token,
    hako_pdu_action_goal_token_t* out_goal_token);
hako_pdu_action_error_t hako_pdu_action_server_reject_goal(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);
hako_pdu_action_error_t hako_pdu_action_server_accept_cancel(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);
hako_pdu_action_error_t hako_pdu_action_server_reject_cancel(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);
hako_pdu_action_error_t hako_pdu_action_server_send_feedback(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_goal_token_t goal_token,
    const uint8_t* pdu,
    size_t pdu_size);
hako_pdu_action_error_t hako_pdu_action_server_complete(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_goal_token_t goal_token,
    hako_pdu_action_terminal_status_t status,
    const uint8_t* pdu,
    size_t pdu_size);

/*
 * TODO(codex): add mux declarations only after Goal ownership across transport
 * sessions is implemented. The static API intentionally lands first.
 *
 * Contract: event_token is one-shot. goal_token is a server-side capability
 * valid from goal acceptance until terminal completion commits. Client users
 * retain a goal handle; they do not manipulate goal_token. Result wins over a
 * pending Cancel by invalidating its event_token and emitting no response.
 */

#ifdef __cplusplus
}
#endif
