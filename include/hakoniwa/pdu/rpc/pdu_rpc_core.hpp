#pragma once

#include "pdu_rpc_types.hpp"
#include "pdu_rpc_time.hpp"
#include <memory>

namespace hakoniwa::pdu::rpc {

// Represents the state of a single RPC request/response transaction
class PduRpcCore {
public:
    PduRpcCore(RequestId req_id, std::shared_ptr<ITimeSource> time_source, uint64_t timeout_usec)
        : request_id_(req_id),
          time_source_(time_source),
          status_(RpcStatus::DOING),
          deadline_usec_(time_source->get_current_time_usec() + timeout_usec) {}

    RequestId get_request_id() const {
        return request_id_;
    }

    RpcStatus get_status() const {
        return status_;
    }

    void set_status(RpcStatus status) {
        status_ = status;
    }

    bool is_timed_out() const {
        if (!time_source_) return true; // Or handle as an error
        return time_source_->get_current_time_usec() > deadline_usec_;
    }

private:
    RequestId request_id_;
    std::shared_ptr<ITimeSource> time_source_;
    RpcStatus status_;
    uint64_t deadline_usec_;
    // PduData request_pdu_; // To be added later
    // PduData response_pdu_; // To be added later
};

} // namespace hakoniwa::pdu::rpc
