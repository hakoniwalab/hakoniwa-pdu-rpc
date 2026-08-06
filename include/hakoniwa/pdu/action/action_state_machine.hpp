#pragma once

#include <cstdint>

namespace hakoniwa::pdu::action {

// State-machine decisions are semantic results. They do not describe a
// transport send result and must not be converted directly into a packet.
enum class TransitionDecision : std::uint8_t {
    ALLOW,
    PROTOCOL_REJECT,
    APPLICATION_API_ERROR,
    INVARIANT_VIOLATION,
    IGNORE,
    IDEMPOTENT,
    DEFER,
};

enum class TransitionEffect : std::uint32_t {
    NONE = 0,
    NOTIFY_CANCEL_REQUEST = 1U << 0,
    SEND_CANCEL_RESPONSE_ACCEPTED = 1U << 1,
    SEND_CANCEL_RESPONSE_REJECTED = 1U << 2,
    SEND_CANCEL_REQUEST = 1U << 3,
    PUBLISH_FEEDBACK = 1U << 4,
    COMMIT_RESULT = 1U << 5,
    RELEASE_GOAL = 1U << 6,
    SEND_PROTOCOL_REJECT = 1U << 7,
    DELIVER_FEEDBACK = 1U << 8,
    DELIVER_RESULT = 1U << 9,
    NOTIFY_RUNTIME_ERROR = 1U << 10,
    DEFER_TO_POLICY = 1U << 11,
    RECORD_DIAGNOSTIC = 1U << 12,
};

// IMMEDIATE transitions are committed before or independently of effects.
// AFTER_EFFECT_SUCCESS transitions are proposals: the owner commits `next`
// only when every required effect succeeds.
enum class TransitionCommit : std::uint8_t {
    IMMEDIATE,
    AFTER_EFFECT_SUCCESS,
};

constexpr TransitionEffect operator|(
    TransitionEffect lhs,
    TransitionEffect rhs) noexcept
{
    return static_cast<TransitionEffect>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool has_effect(
    TransitionEffect effects,
    TransitionEffect expected) noexcept
{
    return (static_cast<std::uint32_t>(effects)
            & static_cast<std::uint32_t>(expected))
        != 0U;
}

} // namespace hakoniwa::pdu::action
