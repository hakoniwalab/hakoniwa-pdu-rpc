#pragma once

#include "pdu_rpc_types.hpp"
#include "hakoniwa/pdu/endpoint.hpp"
#include <string>
#include <memory>
#include <map>
#include <future>
#include <optional>
#include <nlohmann/json_fwd.hpp>

namespace hakoniwa::pdu::rpc {

class PduRpcClientEndpoint; // Forward declaration

// poll() の戻り値
struct RpcClientEvent {
    std::string service_name;
    std::string client_name;
    std::future_status status; // futureの状態
    PduData response_pdu;      // statusがreadyの場合に有効
};

class RpcServicesClient {
public:
    RpcServicesClient(const std::string& node_id, const std::string& config_path, const std::string& impl_type = "PduRpcClientEndpointImpl");
    ~RpcServicesClient(); // stop_all_servicesを呼ぶために実装が必要

    // 対称性: Server.initialize_services
    bool initialize_services();

    // 対称性: Server.start_all_services
    void start_all_services();

    // 対称性: Server.stop_all_services (Server側にも追加を検討)
    void stop_all_services();

    // Client固有の中核機能
    std::future<PduData> call_async(
        const std::string& service_name, 
        const std::string& client_name, 
        const PduData& request_pdu, 
        uint64_t timeout_usec);
    
    // Client固有のイベント取得機能
    std::optional<RpcClientEvent> poll();

private:
    std::string node_id_;
    std::string config_path_;
    std::string impl_type_;

    std::map<std::pair<std::string, std::string>, std::shared_ptr<hakoniwa::pdu::Endpoint>> pdu_endpoints_;
    std::map<std::pair<std::string, std::string>, std::shared_ptr<PduRpcClientEndpoint>> rpc_endpoints_;

    // poll()でチェックするための進行中リクエストのリスト
    //std::vector<std::pair<std::string, std::string>> active_requests_; // 仮
};

} // namespace hakoniwa::pdu::rpc