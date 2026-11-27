#include <hakoniwa_asset_polling.h>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_SystemControlRequestPacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_SystemControlResponsePacket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
constexpr const char* kAsset = "PduRpcShmServer";
constexpr std::array<const char*, 2> kServices{"Service/Add", "Service/Add2"};
constexpr const char* kOpcodeService = "Service/Opcode";
constexpr hako_time_t kDeltaUsec = 1000;
constexpr Hako_int64 kInvalidRequest = -3000;
constexpr Hako_int64 kDelayedRequest = -2000;
constexpr Hako_int64 kNeverRequest = -1000;
constexpr hako_time_t kDelayUsec = 50'000;
std::atomic_bool keep_running{true};

struct PendingRequest {
    bool active{false};
    HakoCpp_AddTwoIntsRequest body{};
    hako_time_t received_at{0};
};

void stop_handler(int)
{
    keep_running = false;
}

bool send_response(int service_id, const PendingRequest& pending, int result_code)
{
    char* response = nullptr;
    size_t response_size = 0;
    HakoCpp_AddTwoIntsResponsePacket out{};
    if (hako_asset_service_server_get_response_buffer(
            service_id,
            &response,
            &response_size,
            HAKO_SERVICE_API_STATUS_DONE,
            result_code) != 0 ||
        !hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket{}.pdu2cpp(response, out)) {
        return false;
    }
    out.body.sum = pending.body.a + pending.body.b;
    if (hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket{}.cpp2pdu(
            out, response, response_size) < 0 ||
        hako_asset_service_server_put_response(service_id, response, response_size) != 0) {
        return false;
    }
    std::cout << "SERVER reply result=" << result_code << " sum=" << out.body.sum << "\n";
    return true;
}

bool receive_request(int service_id, PendingRequest& pending, hako_time_t world_time)
{
    char* request = nullptr;
    size_t request_size = 0;
    HakoCpp_AddTwoIntsRequestPacket in{};
    if (hako_asset_service_server_get_request(service_id, &request, &request_size) != 0 ||
        !hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket{}.pdu2cpp(request, in)) {
        return false;
    }
    pending.active = true;
    pending.body = in.body;
    pending.received_at = world_time;
    std::cout << "SERVER request a=" << in.body.a << " b=" << in.body.b << "\n";
    return true;
}

bool poll_opcode_service(int service_id)
{
    const int event = hako_asset_service_server_poll(service_id);
    if (event != HAKO_SERVICE_SERVER_API_REQUEST_IN) return event >= 0;

    char* request = nullptr;
    size_t request_size = 0;
    HakoCpp_SystemControlRequestPacket in{};
    if (hako_asset_service_server_get_request(service_id, &request, &request_size) != 0 ||
        !hako::pdu::msgs::hako_srv_msgs::SystemControlRequestPacket{}.pdu2cpp(request, in)) {
        return false;
    }
    std::cout << "SERVER received opcode=" << in.body.opcode << "\n";

    char* response = nullptr;
    size_t response_size = 0;
    HakoCpp_SystemControlResponsePacket out{};
    if (hako_asset_service_server_get_response_buffer(
            service_id,
            &response,
            &response_size,
            HAKO_SERVICE_API_STATUS_DONE,
            HAKO_SERVICE_API_RESULT_CODE_OK) != 0 ||
        !hako::pdu::msgs::hako_srv_msgs::SystemControlResponsePacket{}.pdu2cpp(response, out)) {
        return false;
    }
    out.body.status_code = in.body.opcode;
    return hako::pdu::msgs::hako_srv_msgs::SystemControlResponsePacket{}.cpp2pdu(
               out, response, response_size) >= 0 &&
        hako_asset_service_server_put_response(service_id, response, response_size) == 0;
}
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    const bool serve_opcode = argc == 3 && std::string_view(argv[2]) == "request-body-offset";
    if (argc != 2 && !serve_opcode) {
        std::cerr << "Usage: " << argv[0] << " <service.json> [request-body-offset]\n";
        return 2;
    }
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    if (hakoniwa_asset_init() != 0 || hakoniwa_simevent_init() != 0 ||
        hakoniwa_asset_register_polling(kAsset) != 0 ||
        hako_asset_service_initialize(argv[1]) != 0) {
        std::cerr << "SERVER ERROR: initialization failed\n";
        return 1;
    }
    std::cout << "SERVER registered\n";

    std::array<int, kServices.size()> service_ids{-1, -1};
    int opcode_service_id = -1;
    std::array<PendingRequest, kServices.size()> pending{};
    bool started = false;
    hako_time_t local_time = 0;
    while (keep_running) {
        const int asset_event = hakoniwa_asset_get_event(kAsset);
        if (asset_event == 1) {
            if (!hakoniwa_asset_is_pdu_created()) return 1;
            for (std::size_t i = 0; i < kServices.size(); ++i) {
                service_ids[i] = hako_asset_service_server_create(kAsset, kServices[i]);
                if (service_ids[i] < 0) return 1;
                pending[i] = {};
            }
            if (serve_opcode) {
                opcode_service_id = hako_asset_service_server_create(kAsset, kOpcodeService);
                if (opcode_service_id < 0) return 1;
            }
            started = true;
            local_time = 0;
            hakoniwa_asset_notify_write_pdu_done(kAsset);
            if (hakoniwa_asset_start_feedback_ok(kAsset) != 0) return 1;
            std::cout << "SERVER event Start\n";
        } else if (asset_event == 2 || asset_event == 3) {
            started = false;
            pending = {};
            if (asset_event == 2) {
                if (hakoniwa_asset_stop_feedback_ok(kAsset) != 0) return 1;
                std::cout << "SERVER event Stop\n";
            } else {
                local_time = 0;
                if (hakoniwa_asset_reset_feedback_ok(kAsset) != 0) return 1;
                std::cout << "SERVER event Reset\n";
            }
        } else if (asset_event == 4) {
            return 1;
        }

        if (!started || hakoniwa_simevent_get_state() != 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const hako_time_t world_time = hakoniwa_asset_get_worldtime();
        for (std::size_t i = 0; i < service_ids.size(); ++i) {
            const int event = hako_asset_service_server_poll(service_ids[i]);
            if (event == HAKO_SERVICE_SERVER_API_REQUEST_IN) {
                if (!receive_request(service_ids[i], pending[i], world_time)) return 1;
            } else if (event == HAKO_SERVICE_SERVER_API_REQUEST_CANCEL) {
                if (!pending[i].active ||
                    !send_response(service_ids[i], pending[i], HAKO_SERVICE_API_RESULT_CODE_CANCELED)) {
                    return 1;
                }
                pending[i] = {};
            } else if (event < 0) {
                return 1;
            }

            if (!pending[i].active) continue;
            if (pending[i].body.a == kNeverRequest) continue;
            if (pending[i].body.a == kDelayedRequest &&
                world_time < pending[i].received_at + kDelayUsec) {
                continue;
            }
            const int result = pending[i].body.a == kInvalidRequest
                ? HAKO_SERVICE_API_RESULT_CODE_INVALID
                : HAKO_SERVICE_API_RESULT_CODE_OK;
            if (!send_response(service_ids[i], pending[i], result)) return 1;
            pending[i] = {};
        }
        if (serve_opcode && !poll_opcode_service(opcode_service_id)) return 1;
        if (local_time <= world_time) {
            local_time += kDeltaUsec;
            hakoniwa_asset_notify_simtime(kAsset, local_time);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
