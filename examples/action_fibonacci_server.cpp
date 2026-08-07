#include "hakoniwa/pdu/action/action_services_server.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace action = hakoniwa::pdu::action;
using namespace std::chrono_literals;

constexpr const char* kActionName = "fibonacci";
constexpr const char* kNodeId = "fibonacci-server";
volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

bool encode_feedback(
    action::ActionServicesServer& server,
    const std::vector<std::int32_t>& sequence,
    action::PduData& packet)
{
    if (!server.create_feedback_buffer(kActionName, packet)) {
        return false;
    }
    HakoCpp_FibonacciActionFeedback feedback{};
    feedback.body.partial_sequence = sequence;
    hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
    return convertor.cpp2pdu(
               feedback,
               reinterpret_cast<char*>(packet.data()),
               static_cast<int>(packet.size())) > 0;
}

bool encode_result(
    action::ActionServicesServer& server,
    const std::vector<std::int32_t>& sequence,
    action::PduData& packet)
{
    if (!server.create_result_buffer(kActionName, packet)) {
        return false;
    }
    HakoCpp_FibonacciActionResponse response{};
    response.body.sequence = sequence;
    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
    return convertor.cpp2pdu(
               response,
               reinterpret_cast<char*>(packet.data()),
               static_cast<int>(packet.size())) > 0;
}

bool execute_goal(
    action::ActionServicesServer& server,
    action::ServerEvent& event)
{
    HakoCpp_FibonacciActionRequest request{};
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest request_convertor;
    if (!request_convertor.pdu2cpp(
            reinterpret_cast<char*>(event.pdu.data()), request)) {
        std::cerr << "Failed to decode Fibonacci Goal." << std::endl;
        return server.reject_goal(kActionName, event.goal);
    }

    const auto order = request.body.order;
    if (order <= 0 || order > 47) {
        std::cerr << "Rejecting order outside the example range 1..47: "
                  << order << std::endl;
        return server.reject_goal(kActionName, event.goal);
    }
    if (!server.accept_goal(kActionName, event.goal)) {
        std::cerr << "Failed to accept Fibonacci Goal." << std::endl;
        return false;
    }

    std::cout << "Accepted Fibonacci Goal: order=" << order << std::endl;
    std::vector<std::int32_t> sequence;
    sequence.reserve(static_cast<std::size_t>(order));
    sequence.push_back(0);
    if (order >= 2) {
        sequence.push_back(1);
    }

    while (sequence.size() < static_cast<std::size_t>(order)) {
        sequence.push_back(
            sequence[sequence.size() - 1] + sequence[sequence.size() - 2]);

        action::PduData feedback;
        if (!encode_feedback(server, sequence, feedback)
            || !server.send_feedback(kActionName, event.goal, feedback)) {
            std::cerr << "Failed to send Fibonacci Feedback." << std::endl;
            action::PduData result;
            if (encode_result(server, sequence, result)) {
                server.complete(
                    kActionName,
                    event.goal,
                    action::TerminalStatus::ABORTED,
                    result);
            }
            return false;
        }
        std::this_thread::sleep_for(100ms);
    }

    action::PduData result;
    if (!encode_result(server, sequence, result)
        || !server.complete(
            kActionName,
            event.goal,
            action::TerminalStatus::SUCCEEDED,
            result)) {
        std::cerr << "Failed to send Fibonacci Result." << std::endl;
        return false;
    }
    std::cout << "Completed Fibonacci Goal." << std::endl;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 3) {
        std::cerr << "Usage: " << argv[0]
                  << " [resolved-action.json] [endpoints.json]" << std::endl;
        return 1;
    }
    const std::string action_config =
        argc >= 2 ? argv[1] : ".hako/action/resolved-action.json";
    const std::string endpoint_config =
        argc >= 3 ? argv[2] : ".hako/action/endpoints.json";

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        kNodeId, endpoint_config);
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to initialize Action Server endpoints." << std::endl;
        return 1;
    }

    action::ActionServicesServer server(kNodeId, action_config);
    if (!server.initialize_services(endpoints)) {
        std::cerr << "Failed to initialize Action Server services." << std::endl;
        return 1;
    }
    if (endpoints->start_all() != HAKO_PDU_ERR_OK
        || !server.start_all_services()) {
        std::cerr << "Failed to start Action Server." << std::endl;
        return 1;
    }

    std::cout << "Fibonacci Action Server is ready. Press Ctrl+C to stop."
              << std::endl;
    while (!stop_requested) {
        std::string action_name;
        action::ServerEvent event;
        const auto type = server.poll(action_name, event);
        if (type == action::ServerEventType::GOAL_REQUEST) {
            execute_goal(server, event);
        } else if (type == action::ServerEventType::ERROR) {
            std::cerr << "Action Server reported a Runtime error." << std::endl;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    endpoints->stop_all();
    server.stop_all_services();
    server.clear_all_instances();
    return 0;
}
