#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

using PduData = std::vector<std::uint8_t>;
using GoalId = std::array<std::uint8_t, 16>;
using EventToken = std::uint64_t;
using GoalToken = std::uint64_t;

// These values describe logical runtime events. Wire-level values are defined
// by the generated Action headers from hakoniwa-pdu-registry.
enum class ClientEventType {
    NONE,
    GOAL_RESPONSE,
    FEEDBACK,
    CANCEL_RESPONSE,
    RESULT,
    TIMEOUT,
    ERROR
};

enum class ServerEventType {
    NONE,
    GOAL_REQUEST,
    CANCEL_REQUEST,
    ERROR
};

enum class GoalDecision {
    ACCEPTED,
    REJECTED
};

enum class CancelDecision {
    ACCEPTED,
    REJECTED
};

enum class TerminalStatus {
    SUCCEEDED,
    CANCELED,
    ABORTED
};

enum class GoalState {
    EXECUTING,
    CANCELING,
    FINISHING
};

struct ClientEvent {
    ClientEventType type{ClientEventType::NONE};
    std::string action_name;
    GoalId goal_id{};
    GoalDecision goal_decision{GoalDecision::REJECTED};
    CancelDecision cancel_decision{CancelDecision::REJECTED};
    TerminalStatus terminal_status{TerminalStatus::ABORTED};
    std::uint32_t feedback_sequence{0};
    PduData pdu;
};

struct ServerEvent {
    ServerEventType type{ServerEventType::NONE};
    EventToken event_token{0};
    std::string action_name;
    std::string client_name;
    GoalId goal_id{};
    PduData pdu;
};

// TODO(codex): define the precise runtime error model without exposing
// implementation-specific exceptions through the public API.

} // namespace hakoniwa::pdu::action
