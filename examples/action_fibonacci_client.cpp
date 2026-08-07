#include "hakoniwa/pdu/action/action_services_client.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionFeedback.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace action = hakoniwa::pdu::action;
using namespace std::chrono_literals;

constexpr const char* kActionName = "fibonacci";
constexpr const char* kNodeId = "fibonacci-client";
constexpr const char* kClientName = "fibonacci-example-client";

action::GoalId make_goal_id()
{
    action::GoalId id{};
    std::random_device random;
    for (auto& byte : id) {
        byte = static_cast<std::uint8_t>(random());
    }
    if (!action::is_valid_goal_id(id)) {
        id.back() = 1;
    }
    return id;
}

bool parse_order(const char* text, std::int32_t& order)
{
    try {
        const auto value = std::stoll(text);
        if (value <= 0 || value > 47) {
            return false;
        }
        order = static_cast<std::int32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

void print_sequence(const std::vector<std::int32_t>& sequence)
{
    std::cout << '[';
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        if (index != 0) {
            std::cout << ", ";
        }
        std::cout << sequence[index];
    }
    std::cout << ']' << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 4) {
        std::cerr << "Usage: " << argv[0]
                  << " [order] [resolved-action.json] [endpoints.json]"
                  << std::endl;
        return 1;
    }
    std::int32_t order = 10;
    if (argc >= 2 && !parse_order(argv[1], order)) {
        std::cerr << "order must be an integer in the range 1..47."
                  << std::endl;
        return 1;
    }
    const std::string action_config =
        argc >= 3 ? argv[2] : ".hako/action/resolved-action.json";
    const std::string endpoint_config =
        argc >= 4 ? argv[3] : ".hako/action/endpoints.json";

    auto endpoints = std::make_shared<hakoniwa::pdu::EndpointContainer>(
        kNodeId, endpoint_config);
    if (endpoints->initialize() != HAKO_PDU_ERR_OK) {
        std::cerr << "Failed to initialize Action Client endpoints." << std::endl;
        return 1;
    }

    action::ActionServicesClient client(
        kNodeId, kClientName, action_config);
    if (!client.initialize_services(endpoints)) {
        std::cerr << "Failed to initialize Action Client services." << std::endl;
        return 1;
    }
    if (endpoints->start_all() != HAKO_PDU_ERR_OK
        || !client.start_all_services()) {
        std::cerr << "Failed to start Action Client." << std::endl;
        return 1;
    }

    std::cout << "Waiting for the TCP connection..." << std::endl;
    const auto connection_deadline = std::chrono::steady_clock::now() + 5s;
    while (!endpoints->is_running_all()) {
        if (std::chrono::steady_clock::now() >= connection_deadline) {
            std::cerr << "Timed out waiting for the Action Server connection."
                      << std::endl;
            endpoints->stop_all();
            client.stop_all_services();
            client.clear_all_instances();
            return 1;
        }
        std::this_thread::sleep_for(1ms);
    }

    action::PduData goal_packet;
    if (!client.create_goal_buffer(kActionName, goal_packet)) {
        std::cerr << "Failed to create Fibonacci Goal buffer." << std::endl;
        return 1;
    }
    HakoCpp_FibonacciActionRequest request{};
    request.body.order = order;
    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest request_convertor;
    if (request_convertor.cpp2pdu(
            request,
            reinterpret_cast<char*>(goal_packet.data()),
            static_cast<int>(goal_packet.size())) <= 0) {
        std::cerr << "Failed to encode Fibonacci Goal." << std::endl;
        return 1;
    }

    action::ClientGoalHandle goal;
    if (!client.send_goal(
            kActionName, goal_packet, make_goal_id(), goal, 5'000'000)) {
        std::cerr << "Failed to send Fibonacci Goal." << std::endl;
        return 1;
    }
    std::cout << "Sent Fibonacci Goal: order=" << order << std::endl;

    int exit_code = 1;
    bool finished = false;
    while (!finished) {
        std::string action_name;
        action::ClientEvent event;
        const auto type = client.poll(action_name, event);
        switch (type) {
        case action::ClientEventType::NONE:
            std::this_thread::sleep_for(1ms);
            break;
        case action::ClientEventType::GOAL_RESPONSE:
            std::cout << "Goal Response: "
                      << (event.decision == action::Decision::ACCEPTED
                              ? "ACCEPTED"
                              : "REJECTED")
                      << std::endl;
            if (event.decision != action::Decision::ACCEPTED) {
                finished = true;
            }
            break;
        case action::ClientEventType::FEEDBACK: {
            HakoCpp_FibonacciActionFeedback feedback{};
            hako::pdu::msgs::sample_action_msgs::FibonacciActionFeedback convertor;
            if (!convertor.pdu2cpp(
                    reinterpret_cast<char*>(event.pdu.data()), feedback)) {
                std::cerr << "Failed to decode Fibonacci Feedback." << std::endl;
                finished = true;
                break;
            }
            std::cout << "Feedback: ";
            print_sequence(feedback.body.partial_sequence);
            break;
        }
        case action::ClientEventType::RESULT: {
            HakoCpp_FibonacciActionResponse response{};
            hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse convertor;
            if (!convertor.pdu2cpp(
                    reinterpret_cast<char*>(event.pdu.data()), response)) {
                std::cerr << "Failed to decode Fibonacci Result." << std::endl;
                finished = true;
                break;
            }
            std::cout << "Result: ";
            print_sequence(response.body.sequence);
            exit_code = event.terminal_status == action::TerminalStatus::SUCCEEDED
                ? 0
                : 1;
            finished = true;
            break;
        }
        case action::ClientEventType::TIMEOUT:
            std::cerr << "Timed out waiting for Goal Response." << std::endl;
            finished = true;
            break;
        case action::ClientEventType::ERROR:
            std::cerr << "Action Client reported a Runtime error." << std::endl;
            finished = true;
            break;
        case action::ClientEventType::CANCEL_RESPONSE:
            std::cerr << "Unexpected Cancel Response in this example."
                      << std::endl;
            finished = true;
            break;
        }
    }

    endpoints->stop_all();
    client.stop_all_services();
    client.clear_all_instances();
    return exit_code;
}
