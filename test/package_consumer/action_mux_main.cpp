#include "hakoniwa/pdu/action/action_services_mux_server.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"

#include <cstdint>

int main()
{
    HakoCpp_FibonacciActionRequest request{};
    request.body.order = 8;

    hakoniwa::pdu::action::ActionServicesMuxServer server(
        "package-consumer-action-node",
        "unused-action-config.json",
        "unused-endpoint-mux-config.json");

    return request.body.order == 8
            && server.expected_count() == 0
            && !server.is_ready()
        ? 0
        : 1;
}
