#include "hakoniwa/pdu/rpc/rpc_service_helper.hpp"
#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include <hakoniwa_asset_polling.h>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_ServiceResponseHeader.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace hakoniwa::pdu::rpc;

namespace {
using namespace std::chrono_literals;
constexpr const char* kService = "Service/Add";
constexpr const char* kService2 = "Service/Add2";
constexpr Hako_int64 kInvalidRequest = -3000;
constexpr Hako_int64 kDelayedRequest = -2000;
constexpr Hako_int64 kNeverRequest = -1000;

class IntegrationClient {
public:
    IntegrationClient(const char* services, const char* endpoints)
        : client_(
              "shm_client_node",
              "ShmClient",
              services,
              "RpcClientEndpointImpl",
              1000,
              "real",
              endpoints)
    {
    }

    bool initialize()
    {
        return client_.initialize_services(nullptr);
    }

    bool call(const std::string& service, Hako_int64 a, Hako_int64 b, uint64_t timeout, uint64_t& request_id)
    {
        HakoCpp_AddTwoIntsRequest body{};
        body.a = a;
        body.b = b;
        PduData request;
        if (!rpc_.set_request_body(client_, service, body, request)) return false;
        HakoCpp_AddTwoIntsRequestPacket packet{};
        if (!hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket{}.pdu2cpp(
                reinterpret_cast<char*>(request.data()), packet)) {
            return false;
        }
        request_id = packet.header.request_id;
        return client_.call(service, request, timeout);
    }

    ClientEventType wait_event(
        std::string& service,
        RpcResponse& response,
        std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto event = client_.poll(service, response);
            if (event != ClientEventType::NONE) return event;
            std::this_thread::sleep_for(1ms);
        }
        return ClientEventType::NONE;
    }

    bool check_response(
        const RpcResponse& response,
        uint64_t request_id,
        int result,
        Hako_int64 sum)
    {
        RpcResponse copy = response;
        HakoCpp_AddTwoIntsResponse body{};
        return response.header.request_id == request_id &&
            response.header.result_code == result &&
            rpc_.get_response_body(copy, body) && body.sum == sum;
    }

    bool cancel(const std::string& service)
    {
        return client_.send_cancel_request(service);
    }

private:
    RpcServicesClient client_;
    HakoRpcServiceServerTemplateType(AddTwoInts) rpc_;
};

bool wait_file(const std::filesystem::path& path, std::chrono::seconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool mark_file(const std::filesystem::path& path)
{
    std::ofstream output(path);
    output.put('\n');
    return output.good();
}

int wait_direct_client_id(const std::filesystem::path& work)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int id = 0; id < 2; ++id) {
            if (std::filesystem::exists(
                    work / ("direct-client-" + std::to_string(id) + ".ready"))) {
                return id;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    return -1;
}

bool write_body(const std::string& path, const RpcResponse& response)
{
    HakoCpp_ServiceResponseHeader empty{};
    std::vector<uint8_t> header_bytes(512);
    const int header_size = hako::pdu::PduConvertor<
        HakoCpp_ServiceResponseHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader>{}.cpp2pdu(
            empty,
            reinterpret_cast<char*>(header_bytes.data()),
            header_bytes.size());
    if (header_size < 0 || static_cast<std::size_t>(header_size) > response.pdu.size()) return false;
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(response.pdu.data() + header_size),
        static_cast<std::streamsize>(response.pdu.size() - header_size));
    return output.good();
}

bool call_and_check(
    IntegrationClient& client,
    const std::string& service,
    Hako_int64 a,
    Hako_int64 b,
    int result,
    ClientEventType expected_event,
    RpcResponse* output = nullptr)
{
    uint64_t request_id = 0;
    if (!client.call(service, a, b, 5'000'000, request_id)) return false;
    std::string response_service;
    RpcResponse response;
    if (client.wait_event(response_service, response) != expected_event ||
        response_service != service ||
        !client.check_response(response, request_id, result, a + b)) {
        return false;
    }
    if (output) *output = std::move(response);
    return true;
}

bool run_ok(IntegrationClient& client)
{
    if (!call_and_check(
            client,
            kService,
            20,
            22,
            HAKO_SERVICE_RESULT_CODE_OK,
            ClientEventType::RESPONSE_IN)) {
        return false;
    }
    std::cout << "PASS shm core-pro OK sum=42\n";
    return true;
}

bool run_compare(IntegrationClient& client, const std::string& work)
{
    struct Case {
        const char* name;
        Hako_int64 a;
        int result;
        ClientEventType event;
        bool cancel;
    };
    const Case cases[] = {
        {"ok", 20, HAKO_SERVICE_RESULT_CODE_OK, ClientEventType::RESPONSE_IN, false},
        {"invalid", kInvalidRequest, HAKO_SERVICE_RESULT_CODE_INVALID, ClientEventType::RESPONSE_IN, false},
        {"canceled", kNeverRequest, HAKO_SERVICE_RESULT_CODE_CANCELED, ClientEventType::RESPONSE_CANCEL, true},
    };
    const int direct_client_id = wait_direct_client_id(work);
    if (direct_client_id < 0) return false;
    const int client_id = 1 - direct_client_id;
    for (const auto& test : cases) {
        const auto slot0_done = std::filesystem::path(work) /
            (std::string(test.name) + "-slot0.done");
        const auto slot1_done = std::filesystem::path(work) /
            (std::string(test.name) + "-slot1.done");
        if (client_id == 1 && !wait_file(slot0_done)) {
            return false;
        }
        uint64_t request_id = 0;
        if (!client.call(kService, test.a, 22, 0, request_id)) return false;
        if (test.cancel) {
            std::this_thread::sleep_for(20ms);
            if (!client.cancel(kService)) return false;
        }
        std::string service;
        RpcResponse response;
        if (client.wait_event(service, response) != test.event || service != kService ||
            !client.check_response(response, request_id, test.result, test.a + 22) ||
            !write_body(work + "/rpc-" + test.name + ".body", response)) {
            return false;
        }
        if (!mark_file(work + "/rpc-" + test.name + ".done")) return false;
        if (client_id == 0) {
            if (!mark_file(slot0_done) || !wait_file(slot1_done)) return false;
        } else if (!mark_file(slot1_done)) {
            return false;
        }
    }
    std::cout << "PASS S2(a) rpc result bodies and restored request ids\n";
    return true;
}

bool run_500(IntegrationClient& client)
{
    for (int i = 1; i <= 500; ++i) {
        if (!call_and_check(
                client,
                kService,
                i,
                i * 2,
                HAKO_SERVICE_RESULT_CODE_OK,
                ClientEventType::RESPONSE_IN)) {
            std::cerr << "CLIENT ERROR: call " << i << "/500 failed\n";
            return false;
        }
    }
    std::cout << "PASS S2(b) 500/500 consecutive calls aligned\n";
    return true;
}

bool run_busy(IntegrationClient& client, const std::string& work)
{
    const int direct_client_id = wait_direct_client_id(work);
    if (direct_client_id < 0) return false;
    const int client_id = 1 - direct_client_id;
    const auto slot0_done = std::filesystem::path(work) / "busy-slot0.done";
    // The polling server reads slot 1 after slot 0 gets a terminal reply.
    if (client_id == 1 && !wait_file(slot0_done)) return false;
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    if (!client.call(kService, kNeverRequest, 2, 0, first_id) ||
        client.call(kService, 1, 2, 0, second_id)) {
        return false;
    }
    std::this_thread::sleep_for(20ms);
    if (!client.cancel(kService)) return false;
    std::string service;
    RpcResponse response;
    if (client.wait_event(service, response) != ClientEventType::RESPONSE_CANCEL ||
        !client.check_response(
            response,
            first_id,
            HAKO_SERVICE_RESULT_CODE_CANCELED,
            kNeverRequest + 2)) {
        return false;
    }
    if (client_id == 0 && !mark_file(slot0_done)) return false;
    std::cout << "PASS S2(c) second call refused before core-pro\n";
    return true;
}

bool run_timeout(IntegrationClient& client)
{
    uint64_t request_id = 0;
    if (!client.call(kService, kNeverRequest, 3, 20'000, request_id)) return false;
    std::string service;
    RpcResponse response;
    if (client.wait_event(service, response) != ClientEventType::RESPONSE_TIMEOUT ||
        service != kService || !client.cancel(kService)) {
        return false;
    }
    response = {};
    if (client.wait_event(service, response) != ClientEventType::RESPONSE_CANCEL ||
        !client.check_response(
            response,
            request_id,
            HAKO_SERVICE_RESULT_CODE_CANCELED,
            kNeverRequest + 3) ||
        !call_and_check(
            client,
            kService,
            7,
            8,
            HAKO_SERVICE_RESULT_CODE_OK,
            ClientEventType::RESPONSE_IN)) {
        return false;
    }
    std::cout << "PASS S2(d) timeout -> cancel -> CANCELED -> next OK\n";
    return true;
}

bool run_time_advance(IntegrationClient& client)
{
    uint64_t request_id = 0;
    const hako_time_t before = hakoniwa_asset_get_worldtime();
    if (!client.call(kService, kDelayedRequest, 5, 5'000'000, request_id)) return false;
    hako_time_t greatest = before;
    std::string service;
    RpcResponse response;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ClientEventType event = ClientEventType::NONE;
    while (std::chrono::steady_clock::now() < deadline) {
        greatest = std::max(greatest, hakoniwa_asset_get_worldtime());
        event = client.wait_event(service, response, 1ms);
        if (event != ClientEventType::NONE) break;
    }
    if (event != ClientEventType::RESPONSE_IN || greatest < before + 10'000 ||
        !client.check_response(
            response,
            request_id,
            HAKO_SERVICE_RESULT_CODE_OK,
            kDelayedRequest + 5)) {
        return false;
    }
    std::cout << "PASS S2(e) client asset time advanced during server delay by "
              << greatest - before << " usec\n";
    return true;
}

bool run_multi_endpoint(IntegrationClient& client)
{
    if (!call_and_check(
            client,
            kService,
            3,
            4,
            HAKO_SERVICE_RESULT_CODE_OK,
            ClientEventType::RESPONSE_IN) ||
        !call_and_check(
            client,
            kService2,
            5,
            6,
            HAKO_SERVICE_RESULT_CODE_OK,
            ClientEventType::RESPONSE_IN)) {
        return false;
    }
    std::cout << "PASS S2(f) two shm endpoints shared one polling asset\n";
    return true;
}

bool run_restart(IntegrationClient& client, const std::string& work)
{
    uint64_t request_id = 0;
    if (!client.call(kService, kNeverRequest, 9, 0, request_id)) return false;
    std::cout << "CLIENT restart request queued\n";
    std::string service;
    RpcResponse response;
    if (client.wait_event(service, response, 20s) != ClientEventType::RESPONSE_IN ||
        service != kService || response.header.request_id != request_id ||
        response.header.result_code != HAKO_SERVICE_RESULT_CODE_ERROR) {
        return false;
    }
    std::ofstream(work + "/restart-error.done").put('\n');
    std::cout << "PASS S2(g) in-flight call returned error on stop\n";
    if (!wait_file(std::filesystem::path(work) / "restart-go", 20s) ||
        !call_and_check(
            client,
            kService,
            40,
            2,
            HAKO_SERVICE_RESULT_CODE_OK,
            ClientEventType::RESPONSE_IN)) {
        return false;
    }
    std::cout << "PASS S2(g) call succeeded after reset and start\n";
    return true;
}
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <service.json> <endpoints.json> <scenario> <work-dir>\n";
        return 2;
    }
    IntegrationClient client(argv[1], argv[2]);
    if (!client.initialize()) return 1;
    std::cout << "CLIENT registered\n";
    const std::string scenario = argv[3];
    const std::string work = argv[4];
    if (!wait_file(std::filesystem::path(work) / "start-go")) return 1;
    bool passed = false;
    if (scenario == "ok") passed = run_ok(client);
    else if (scenario == "compare") passed = run_compare(client, work);
    else if (scenario == "calls500") passed = run_500(client);
    else if (scenario == "busy") passed = run_busy(client, work);
    else if (scenario == "timeout") passed = run_timeout(client);
    else if (scenario == "time") passed = run_time_advance(client);
    else if (scenario == "multi") passed = run_multi_endpoint(client);
    else if (scenario == "restart") passed = run_restart(client, work);
    else return 2;
    return passed ? 0 : 1;
}
