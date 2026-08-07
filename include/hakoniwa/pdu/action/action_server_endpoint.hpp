#pragma once

#include "action_types.hpp"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <utility>

namespace hakoniwa::pdu::action {

// Terminal completion has a commit point before the Result wire send. This
// outcome distinguishes retryable validation rejection from an indeterminate
// delivery failure after that commit.
enum class CompleteResult : std::uint8_t {
    NOT_COMMITTED,
    SENT,
    SEND_FAILED_AFTER_COMMIT,
};

class IActionServerEndpoint {
public:
    virtual ~IActionServerEndpoint() = default;

    virtual bool initialize(const nlohmann::json& action_config) = 0;

    virtual ServerEventType poll(ServerEvent& event_out) = 0;

    // The typed ServerGoalHandle identifies the Goal across Goal decision,
    // Cancel decision, Feedback, and terminal completion. No separate event or
    // goal token is exposed through the Native API.
    virtual bool accept_goal(const ServerGoalHandle& goal) = 0;
    virtual bool reject_goal(const ServerGoalHandle& goal) = 0;
    virtual bool accept_cancel(const ServerGoalHandle& goal) = 0;
    virtual bool reject_cancel(const ServerGoalHandle& goal) = 0;
    // Runtime-origin Cancel has no wire requester, but the packet binding must
    // still permit a later CANCELED Result.
    virtual bool accept_cancel_locally(const ServerGoalHandle& goal) = 0;

    // Allocate and initialize a complete generated Action packet using the
    // configured heap capacity. The upper typed layer encodes the body into
    // this buffer. The Runtime sends the convertor-produced metadata.total_size;
    // the caller does not need to resize the capacity buffer.
    virtual bool create_result_buffer(PduData& pdu_out) = 0;
    virtual bool create_feedback_buffer(PduData& pdu_out) = 0;

    virtual bool send_feedback(const ServerGoalHandle& goal,
                               const PduData& feedback_pdu) = 0;
    virtual CompleteResult complete(const ServerGoalHandle& goal,
                                    TerminalStatus status,
                                    const PduData& result_pdu) = 0;

    // Validate and release a terminal Goal after its transport connection is
    // known to be unavailable. No wire Result is attempted.
    virtual bool complete_locally(const ServerGoalHandle& goal,
                                  TerminalStatus status,
                                  const PduData& result_pdu) = 0;

    // Invalidate one Goal Request that was exposed to the Application but was
    // never accepted. Active Goal bindings are intentionally not affected.
    virtual bool discard_pending_goal(const ServerGoalHandle& goal) = 0;

    virtual void clear_pending_events() = 0;

    // Clears all Goal contexts and slot ownership after the underlying
    // transport has been stopped or disconnected.
    virtual void reset_contexts() = 0;
    const std::string& get_action_name() const { return action_name_; }

protected:
    IActionServerEndpoint(std::string action_name,
                          std::uint64_t delta_time_usec)
        : action_name_(std::move(action_name)),
          delta_time_usec_(delta_time_usec) {}

    std::string action_name_;
    std::uint64_t delta_time_usec_;
};

// Invariant for the implementation:
// COMPLETE_SUCCEEDED and ACCEPT_CANCEL must commit atomically per Goal. If the
// Result wins, the pending Cancel decision is closed and no Cancel Response is
// emitted. See docs/design/action/10-cancel-result-race.md.

} // namespace hakoniwa::pdu::action
