#pragma once

#include "action_services_server.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hakoniwa::pdu::action {

// Action Server transport owner for EndpointCommMultiplexer sessions.
//
// The Application-facing identity remains action_name + ServerGoalHandle.
// Connection identity is an internal routing detail and is never exposed by
// this API.
class ActionServicesMuxServer {
public:
    ActionServicesMuxServer(
        std::string node_id,
        std::string action_config_path,
        std::string endpoint_mux_config_path,
        std::string impl_type = "ActionServerEndpointImpl",
        std::uint64_t delta_time_usec = 1000,
        std::string time_source_type = "real");
    ~ActionServicesMuxServer();

    ActionServicesMuxServer(const ActionServicesMuxServer&) = delete;
    ActionServicesMuxServer& operator=(const ActionServicesMuxServer&) = delete;

    bool initialize();
    bool start();
    void stop();

    ServerEventType poll(std::string& action_name, ServerEvent& event_out);

    bool accept_goal(
        const std::string& action_name,
        const ServerGoalHandle& goal);
    bool reject_goal(
        const std::string& action_name,
        const ServerGoalHandle& goal);
    bool accept_cancel(
        const std::string& action_name,
        const ServerGoalHandle& goal);
    bool reject_cancel(
        const std::string& action_name,
        const ServerGoalHandle& goal);
    bool create_feedback_buffer(
        const std::string& action_name,
        PduData& pdu_out);
    bool create_result_buffer(
        const std::string& action_name,
        PduData& pdu_out);
    bool send_feedback(
        const std::string& action_name,
        const ServerGoalHandle& goal,
        const PduData& feedback_pdu);
    bool complete(
        const std::string& action_name,
        const ServerGoalHandle& goal,
        TerminalStatus status,
        const PduData& result_pdu);

    std::size_t connected_count() const;
    std::size_t expected_count() const;
    bool is_ready() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hakoniwa::pdu::action
