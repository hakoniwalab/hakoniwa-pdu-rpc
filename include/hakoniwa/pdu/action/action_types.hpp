#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hakoniwa::pdu::action {

using PduData = std::vector<std::uint8_t>;
using GoalId = std::array<std::uint8_t, 16>;

inline bool is_valid_goal_id(const GoalId& goal_id) noexcept {
    for (auto byte : goal_id) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

// Client-side typed handle for a Goal identity.
//
// The upper application supplies the GoalId to send_goal(). The Runtime returns
// it in this handle, and the same value is used for cancel
// requests and Client event correlation. GoalId remains directly available to
// adapters that must preserve an external protocol identity, such as a ROS 2
// Action bridge.
//
// The all-zero GoalId is reserved as invalid. The Runtime does not generate
// GoalIds; invalid or already-active IDs must be rejected synchronously.
struct ClientGoalHandle {
    GoalId goal_id{};

    bool valid() const noexcept { return is_valid_goal_id(goal_id); }
};

// Server-side typed handle for a Goal identity.
//
// GOAL_REQUEST carries a valid GoalId before the Goal is accepted.
// CANCEL_REQUEST and RUNTIME_CANCEL_REQUEST carry the same GoalId for an
// already accepted Goal.
//
// ClientGoalHandle and ServerGoalHandle intentionally remain distinct types
// so that client-side and server-side API values cannot be mixed accidentally.
struct ServerGoalHandle {
    GoalId goal_id{};

    bool valid() const noexcept {
        return is_valid_goal_id(goal_id);
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
    ServerGoalHandle goal;
    RuntimeCancelCause runtime_cancel_cause{RuntimeCancelCause::UNSPECIFIED};
    std::string action_name;
    PduData pdu;
};

// Event field contract:
//   Client events:          goal identifies the correlated Goal.
//   GOAL_REQUEST:           goal contains the requested non-zero GoalId.
//   CANCEL_REQUEST:         goal identifies the accepted target Goal.
//   RUNTIME_CANCEL_REQUEST: goal identifies the target Goal and cause is set.
//
// The Native API uses the typed Goal handles above as lifecycle identities. It
// does not expose separate event or goal tokens.

// TODO(codex): define the precise runtime error model without exposing
// implementation-specific exceptions through the public API.

} // namespace hakoniwa::pdu::action
