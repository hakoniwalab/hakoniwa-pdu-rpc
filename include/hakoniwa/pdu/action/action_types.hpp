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

inline constexpr EventToken INVALID_EVENT_TOKEN = 0;
inline constexpr GoalToken INVALID_GOAL_TOKEN = 0;

// High-level client code should retain this handle rather than manipulate a
// GoalId directly. GoalId remains available for adapters that must preserve an
// external protocol identity, such as a ROS 2 Action bridge.
struct ClientGoalHandle {
    GoalId goal_id{};

    bool valid() const noexcept {
        for (auto byte : goal_id) {
            if (byte != 0) {
                return true;
            }
        }
        return false;
    }
};

// Server-side long-lived handle. event_token is intentionally not included:
// it is a one-shot decision token, while goal_token survives until terminal
// completion commits.
struct ServerGoalHandle {
    GoalId goal_id{};
    GoalToken goal_token{INVALID_GOAL_TOKEN};

    bool valid() const noexcept { return goal_token != INVALID_GOAL_TOKEN; }
};

// These values describe logical runtime events. Wire-level values are defined
// by the generated Action headers from hakoniwa-pdu-registry.
enum class ClientEventType : std::uint8_t {
    UNSPECIFIED = 0,
    NONE = 0,
    GOAL_RESPONSE = 1,
    FEEDBACK = 2,
    CANCEL_RESPONSE = 3,
    RESULT = 4,
    TIMEOUT = 5,
    ERROR = 6
};

enum class ServerEventType : std::uint8_t {
    UNSPECIFIED = 0,
    NONE = 0,
    GOAL_REQUEST = 1,
    CANCEL_REQUEST = 2,
    RUNTIME_CANCEL_REQUEST = 3,
    ERROR = 4
};

enum class GoalDecision : std::uint8_t {
    UNSPECIFIED = 0,
    ACCEPTED = 1,
    REJECTED = 2
};

enum class CancelDecision : std::uint8_t {
    UNSPECIFIED = 0,
    ACCEPTED = 1,
    REJECTED = 2
};

enum class TerminalStatus : std::uint8_t {
    UNSPECIFIED = 0,
    SUCCEEDED = 1,
    CANCELED = 2,
    ABORTED = 3
};

enum class GoalState : std::uint8_t {
    UNSPECIFIED = 0,
    EXECUTING = 1,
    CANCELING = 2,
    FINISHING = 3
};

enum class RuntimeCancelCause : std::uint8_t {
    UNSPECIFIED = 0,
    TRANSPORT_DISCONNECTED = 1,
    SERVER_SHUTDOWN = 2,
    INTERNAL_POLICY = 3
};

struct ClientEvent {
    ClientEventType type{ClientEventType::NONE};
    std::string action_name;
    ClientGoalHandle goal;
    GoalDecision goal_decision{GoalDecision::UNSPECIFIED};
    CancelDecision cancel_decision{CancelDecision::UNSPECIFIED};
    TerminalStatus terminal_status{TerminalStatus::UNSPECIFIED};
    std::uint32_t feedback_sequence{0};
    PduData pdu;
};

struct ServerEvent {
    ServerEventType type{ServerEventType::NONE};
    EventToken event_token{INVALID_EVENT_TOKEN};
    ServerGoalHandle goal;
    RuntimeCancelCause runtime_cancel_cause{RuntimeCancelCause::UNSPECIFIED};
    std::string action_name;
    std::string client_name;
    PduData pdu;
};

// Event field contract:
//   GOAL_REQUEST:           goal.goal_token == INVALID_GOAL_TOKEN
//   CANCEL_REQUEST:         goal identifies the accepted target Goal
//   RUNTIME_CANCEL_REQUEST: goal identifies the target Goal and cause is set
// event_token is consumed by exactly one accept/reject operation.

// TODO(codex): define the precise runtime error model without exposing
// implementation-specific exceptions through the public API.

} // namespace hakoniwa::pdu::action
