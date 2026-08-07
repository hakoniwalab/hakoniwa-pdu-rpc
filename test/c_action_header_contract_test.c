#include "hakoniwa/pdu/action/c_action.h"

#include <stddef.h>

_Static_assert(
    sizeof(hako_pdu_action_goal_id_t) == HAKO_PDU_ACTION_GOAL_ID_SIZE,
    "Action Goal ID ABI must remain 16 bytes");
_Static_assert(
    sizeof(((hako_pdu_action_client_goal_handle_t*)0)->goal_id)
        == HAKO_PDU_ACTION_GOAL_ID_SIZE,
    "Client Goal Handle must contain one Goal ID");
_Static_assert(
    sizeof(((hako_pdu_action_server_goal_handle_t*)0)->goal_id)
        == HAKO_PDU_ACTION_GOAL_ID_SIZE,
    "Server Goal Handle must contain one Goal ID");
_Static_assert(
    HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL == 9,
    "Duplicate Goal error ABI changed");
_Static_assert(
    HAKO_PDU_ACTION_ERROR_NO_FREE_SLOT == 10,
    "No-free-slot error ABI changed");
_Static_assert(
    HAKO_PDU_ACTION_ERROR_INVALID_PACKET == 11,
    "Invalid-packet error ABI changed");

int main(void)
{
    hako_pdu_action_buffer_free(NULL);
    return HAKO_PDU_ACTION_OK == 0 ? 0 : 1;
}
