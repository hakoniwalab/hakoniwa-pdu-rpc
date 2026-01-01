#pragma once

#include "pdu_rpc_types.hpp"
#include "pdu_rpc_time.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <memory>
#include <stdexcept>
#include <limits>

namespace hakoniwa::pdu::rpc {

class PduRpcCore {
public:
    PduRpcCore(RequestId req_id,
               const std::string& client_name, // Added client_name
               std::shared_ptr<ITimeSource> time_source,
               std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint = nullptr)
        : request_id_(req_id),
          client_name_(client_name), // Initialize client_name
          time_source_(std::move(time_source)),
          status_(RpcStatus::DOING),
          deadline_usec_(0),
          endpoint_(std::move(endpoint))
    {
        if (!time_source_) {
            throw std::invalid_argument("PduRpcCore: time_source is null");
        }
    }

    RequestId get_request_id() const { return request_id_; }
    const std::string& get_client_name() const { return client_name_; } // Added getter
    RpcStatus get_status() const { return status_; }
    void set_status(RpcStatus status) { status_ = status; }

    bool is_timed_out() const {
        if (deadline_usec_ == 0) {
            return false;
        }
        return time_source_->get_current_time_usec() >= deadline_usec_;
    }
    void start_timeout(uint64_t timeout_usec) {
        if (timeout_usec == 0) {
            deadline_usec_ = std::numeric_limits<uint64_t>::max();
            return;
        }
        const uint64_t now = time_source_->get_current_time_usec();
        if (now > std::numeric_limits<uint64_t>::max() - timeout_usec) {
            deadline_usec_ = std::numeric_limits<uint64_t>::max();
        } else {
            deadline_usec_ = now + timeout_usec;
        }
    }

    const std::shared_ptr<hakoniwa::pdu::Endpoint>& get_endpoint() const {
        return endpoint_;
    }

private:
    RequestId request_id_;
    std::string client_name_; // Added member
    std::shared_ptr<ITimeSource> time_source_;
    RpcStatus status_;
    uint64_t deadline_usec_;
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
};

} // namespace hakoniwa::pdu::rpc
