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

inline bool is_valid_goal_id(const GoalId& goal_id) noexcept {
    for (auto byte : goal_id) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

// High-level client code should retain this handle rather than manipulate a
// GoalId directly. GoalId remains available for adapters that must preserve an
// external protocol identity, such as a ROS 2 Action bridge.
//
// The all-zero GoalId is reserved as invalid. Runtime-generated IDs must be
// non-zero, and an explicitly requested all-zero ID must be rejected.
struct ClientGoalHandle {
    GoalId goal_id{};

    bool valid() const noexcept { return is_valid_goal_id(goal_id); }
};

// Server event metadata for an accepted Goal. This groups the wire identity
// and the Runtime-local capability so an Application can identify the target
// of CANCEL_REQUEST and RUNTIME_CANCEL_REQUEST events.
//
// Server operations continue to use goal_token directly. This type is not a
// replacement for event_token, which remains a one-shot decision token.
struct ServerGoalHandle {
    GoalId goal_id{};
    GoalToken goal_token{INVALID_GOAL_TOKEN};

    bool valid() const noexcept {
        return is_valid_goal_id(goal_id) && goal_token != INVALID_GOAL_TOKEN;
    }
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

// GOAL_RESPONSE and CANCEL_RESPONSE share the same accepted/rejected value
// domain. The event type determines which decision this field represents.
enum class Decision : std::uint8_t {
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
    Decision decision{Decision::UNSPECIFIED};
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
//                           and goal.goal_id is non-zero
//   CANCEL_REQUEST:         goal identifies the accepted target Goal
//   RUNTIME_CANCEL_REQUEST: goal identifies the target Goal and cause is set
// event_token is consumed by exactly one accept/reject operation.

// TODO(codex): define the precise runtime error model without exposing
// implementation-specific exceptions through the public API.

} // namespace hakoniwa::pdu::action
