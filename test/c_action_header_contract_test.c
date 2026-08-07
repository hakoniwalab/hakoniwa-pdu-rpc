#include "hakoniwa/pdu/action/c_action.h"

#include <stddef.h>

/*
 * Keep this header contract valid for C compilers that do not implement the
 * C11 _Static_assert keyword (notably MSVC's C front-end).  A negative array
 * bound is still a compile-time error, so these remain ABI compile checks.
 */
#define HAKO_C_STATIC_ASSERT(name, condition) \
    typedef char hako_c_static_assert_##name[(condition) ? 1 : -1]

HAKO_C_STATIC_ASSERT(
    goal_id_size,
    sizeof(hako_pdu_action_goal_id_t) == HAKO_PDU_ACTION_GOAL_ID_SIZE);
HAKO_C_STATIC_ASSERT(
    client_goal_handle_size,
    sizeof(((hako_pdu_action_client_goal_handle_t*)0)->goal_id)
        == HAKO_PDU_ACTION_GOAL_ID_SIZE);
HAKO_C_STATIC_ASSERT(
    server_goal_handle_size,
    sizeof(((hako_pdu_action_server_goal_handle_t*)0)->goal_id)
        == HAKO_PDU_ACTION_GOAL_ID_SIZE);
HAKO_C_STATIC_ASSERT(
    duplicate_goal_error_value,
    HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL == 9);
HAKO_C_STATIC_ASSERT(
    no_free_slot_error_value,
    HAKO_PDU_ACTION_ERROR_NO_FREE_SLOT == 10);
HAKO_C_STATIC_ASSERT(
    invalid_packet_error_value,
    HAKO_PDU_ACTION_ERROR_INVALID_PACKET == 11);

#undef HAKO_C_STATIC_ASSERT

int main(void)
{
    hako_pdu_action_buffer_free(NULL);
    hako_pdu_action_mux_server_destroy(NULL);
    if (hako_pdu_action_mux_server_connected_count(NULL) != 0
        || hako_pdu_action_mux_server_expected_count(NULL) != 0
        || hako_pdu_action_mux_server_is_ready(NULL) != 0) {
        return 1;
    }
    return HAKO_PDU_ACTION_OK == 0 ? 0 : 1;
}
