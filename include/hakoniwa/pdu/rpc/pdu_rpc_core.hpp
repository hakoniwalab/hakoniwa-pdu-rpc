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
    PduRpcCore(const std::string& service_name,
               std::shared_ptr<ITimeSource> time_source,
               std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint = nullptr)
        : service_name_(service_name),
          client_name_(""),
          time_source_(std::move(time_source)),
          endpoint_(std::move(endpoint))
    {
        if (!time_source_) {
            throw std::invalid_argument("PduRpcCore: time_source is null");
        }
    }
    const void set_client_name(const std::string& client_name) {
        client_name_ = client_name;
    }
    const std::string& get_service_name() const { return service_name_; }
    const std::string& get_client_name() const { return client_name_; }

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
    std::string service_name_;
    std::string client_name_;
    std::shared_ptr<ITimeSource> time_source_;
    uint64_t deadline_usec_;
    std::shared_ptr<hakoniwa::pdu::Endpoint> endpoint_;
};

} // namespace hakoniwa::pdu::rpc
