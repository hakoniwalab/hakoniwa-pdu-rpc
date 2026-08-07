#include "hakoniwa/pdu/action/c_action.h"

int main(void)
{
    hako_pdu_action_mux_server_handle_t* handle = NULL;
    hako_pdu_action_mux_server_destroy(handle);
    return hako_pdu_action_mux_server_connected_count(handle) == 0
            && hako_pdu_action_mux_server_expected_count(handle) == 0
            && hako_pdu_action_mux_server_is_ready(handle) == 0
        ? 0
        : 1;
}
