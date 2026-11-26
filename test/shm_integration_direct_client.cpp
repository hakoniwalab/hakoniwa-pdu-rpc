#include <hakoniwa_asset_polling.h>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_ServiceResponseHeader.hpp>
#include <pdu_convertor.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr const char* kAsset = "PduRpcDirectClient";
constexpr const char* kService = "Service/Add";
constexpr const char* kClient = "DirectClient";
constexpr hako_time_t kDeltaUsec = 1000;
constexpr Hako_int64 kInvalidRequest = -3000;
constexpr Hako_int64 kNeverRequest = -1000;
constexpr std::size_t kResponseSize = 296;
std::atomic_bool keep_running{true};

void stop_handler(int)
{
    keep_running = false;
}

void advance_time(hako_time_t& local_time)
{
    if (local_time <= hakoniwa_asset_get_worldtime()) {
        local_time += kDeltaUsec;
        hakoniwa_asset_notify_simtime(kAsset, local_time);
    }
}

bool start_call(HakoServiceHandleType& handle, Hako_int64 a, Hako_int64 b)
{
    char* packet = nullptr;
    size_t packet_size = 0;
    HakoCpp_AddTwoIntsRequestPacket request{};
    auto converter = hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket{};
    if (hako_asset_service_client_get_request_buffer(
            &handle,
            &packet,
            &packet_size,
            HAKO_SERVICE_CLIENT_API_OPCODE_REQUEST,
            0) != 0 ||
        !converter.pdu2cpp(packet, request)) {
        return false;
    }
    request.body.a = a;
    request.body.b = b;
    return converter.cpp2pdu(request, packet, packet_size) >= 0 &&
        hako_asset_service_client_call_request(&handle, packet, packet_size, 0) == 0;
}

bool wait_response(
    HakoServiceHandleType& handle,
    hako_time_t& local_time,
    bool cancel,
    std::vector<uint8_t>& response)
{
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(5);
    bool cancel_sent = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel && !cancel_sent &&
            std::chrono::steady_clock::now() - started > std::chrono::milliseconds(20)) {
            if (hako_asset_service_client_cancel_request(&handle) != 0) return false;
            cancel_sent = true;
        }
        const int event = hako_asset_service_client_poll(&handle);
        if (event == HAKO_SERVICE_CLIENT_API_RESPONSE_IN ||
            event == HAKO_SERVICE_CLIENT_API_REQUEST_CANCEL_DONE) {
            char* packet = nullptr;
            size_t packet_size = 0;
            if (hako_asset_service_client_get_response(&handle, &packet, &packet_size, 0) != 0) {
                return false;
            }
            response.assign(packet, packet + packet_size);
            return true;
        }
        if (event == HAKO_SERVICE_CLIENT_API_REQUEST_TIMEOUT || event < 0) return false;
        advance_time(local_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool write_body(const std::string& path, const std::vector<uint8_t>& response)
{
    HakoCpp_ServiceResponseHeader empty{};
    std::vector<uint8_t> header_bytes(512);
    const int header_size = hako::pdu::PduConvertor<
        HakoCpp_ServiceResponseHeader,
        hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader>{}.cpp2pdu(
            empty,
            reinterpret_cast<char*>(header_bytes.data()),
            header_bytes.size());
    if (header_size < 0 || static_cast<std::size_t>(header_size) > response.size()) return false;
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(response.data() + header_size),
        static_cast<std::streamsize>(response.size() - header_size));
    return output.good();
}

bool check_result(const std::vector<uint8_t>& response, int expected)
{
    HakoCpp_ServiceResponseHeader header{};
    return hako::pdu::PduConvertor<
               HakoCpp_ServiceResponseHeader,
               hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader>{}.pdu2cpp(
               reinterpret_cast<char*>(const_cast<uint8_t*>(response.data())), header) &&
        header.result_code == expected;
}

bool wait_file(const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool run_compare(HakoServiceHandleType& handle, hako_time_t& local_time, const std::string& work)
{
    struct Case {
        const char* name;
        Hako_int64 a;
        int result;
        bool cancel;
    };
    const Case cases[] = {
        {"ok", 20, HAKO_SERVICE_API_RESULT_CODE_OK, false},
        {"invalid", kInvalidRequest, HAKO_SERVICE_API_RESULT_CODE_INVALID, false},
        {"canceled", kNeverRequest, HAKO_SERVICE_API_RESULT_CODE_CANCELED, true},
    };
    for (const auto& test : cases) {
        std::vector<uint8_t> response;
        if (!start_call(handle, test.a, 22) ||
            !wait_response(handle, local_time, test.cancel, response) ||
            !check_result(response, test.result) ||
            !write_body(work + "/direct-" + test.name + ".body", response)) {
            return false;
        }
        std::ofstream(work + "/direct-" + test.name + ".done").put('\n');
        if (!wait_file(std::filesystem::path(work) / (std::string("rpc-") + test.name + ".done"))) {
            return false;
        }
    }
    std::ofstream(work + "/direct.done").put('\n');
    std::cout << "PASS direct polling comparison responses\n";
    return true;
}

bool run_busy(HakoServiceHandleType& handle, hako_time_t& local_time, const std::string& work)
{
    char* packet = nullptr;
    size_t packet_size = 0;
    HakoCpp_AddTwoIntsRequestPacket request{};
    auto converter = hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket{};
    if (hako_asset_service_client_get_request_buffer(
            &handle,
            &packet,
            &packet_size,
            HAKO_SERVICE_CLIENT_API_OPCODE_REQUEST,
            0) != 0 ||
        !converter.pdu2cpp(packet, request)) {
        return false;
    }
    request.body.a = kNeverRequest;
    request.body.b = 1;
    if (converter.cpp2pdu(request, packet, packet_size) < 0) return false;
    std::vector<uint8_t> duplicate(packet, packet + packet_size);
    if (hako_asset_service_client_call_request(&handle, packet, packet_size, 0) != 0) return false;

    for (int i = 0; i < 20; ++i) {
        advance_time(local_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!converter.pdu2cpp(reinterpret_cast<char*>(duplicate.data()), request)) return false;
    request.header.request_id += 100;
    if (converter.cpp2pdu(
            request,
            reinterpret_cast<char*>(duplicate.data()),
            duplicate.size()) < 0) {
        return false;
    }
    int request_channel = -1;
    int response_channel = -1;
    if (hako_asset_service_client_get_channel_id_by_handle(
            &handle, &request_channel, &response_channel) != 0 ||
        hakoniwa_asset_write_pdu(
            kAsset,
            kService,
            request_channel,
            reinterpret_cast<const char*>(duplicate.data()),
            duplicate.size()) != 0) {
        return false;
    }
    hakoniwa_asset_notify_write_pdu_done(kAsset);

    std::vector<uint8_t> busy(kResponseSize);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hakoniwa_asset_is_pdu_dirty(kAsset, kService, response_channel) == 1 &&
            hakoniwa_asset_read_pdu(
                kAsset,
                kService,
                response_channel,
                reinterpret_cast<char*>(busy.data()),
                busy.size()) == 0) {
            hakoniwa_asset_notify_read_pdu_done(kAsset);
            break;
        }
        advance_time(local_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    HakoCpp_ServiceResponseHeader busy_header{};
    if (!hako::pdu::PduConvertor<
            HakoCpp_ServiceResponseHeader,
            hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader>{}.pdu2cpp(
            reinterpret_cast<char*>(busy.data()), busy_header) ||
        busy_header.request_id != request.header.request_id ||
        busy_header.result_code != HAKO_SERVICE_API_RESULT_CODE_BUSY ||
        busy_header.status != HAKO_SERVICE_API_STATUS_DOING) {
        return false;
    }
    if (hako_asset_service_client_poll(&handle) != HAKO_SERVICE_CLIENT_API_EVENT_NONE ||
        hako_asset_service_client_cancel_request(&handle) != 0) {
        return false;
    }
    std::vector<uint8_t> canceled;
    if (!wait_response(handle, local_time, false, canceled) ||
        !check_result(canceled, HAKO_SERVICE_API_RESULT_CODE_CANCELED)) {
        return false;
    }
    std::ofstream(work + "/direct-busy.done").put('\n');
    std::cout << "PASS direct core-pro BUSY interim path\n";
    return true;
}
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <service.json> <compare|busy> <work-dir>\n";
        return 2;
    }
    const std::string scenario = argv[2];
    const std::string work = argv[3];
    if (scenario != "compare" && scenario != "busy") return 2;
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    if (hakoniwa_asset_init() != 0 || hakoniwa_simevent_init() != 0 ||
        hakoniwa_asset_register_polling(kAsset) != 0 ||
        hako_asset_service_initialize(argv[1]) != 0) {
        return 1;
    }
    std::cout << "DIRECT registered\n";

    HakoServiceHandleType handle{-1, -1};
    hako_time_t local_time = 0;
    bool started = false;
    bool finished = false;
    while (keep_running) {
        const int event = hakoniwa_asset_get_event(kAsset);
        if (event == 1) {
            if (!hakoniwa_asset_is_pdu_created() ||
                hako_asset_service_client_create(kAsset, kService, kClient, &handle) != 0) {
                return 1;
            }
            local_time = 0;
            started = true;
            hakoniwa_asset_notify_write_pdu_done(kAsset);
            if (hakoniwa_asset_start_feedback_ok(kAsset) != 0) return 1;
        } else if (event == 2 || event == 3) {
            started = false;
            if (event == 2) {
                if (hakoniwa_asset_stop_feedback_ok(kAsset) != 0) return 1;
            } else {
                local_time = 0;
                if (hakoniwa_asset_reset_feedback_ok(kAsset) != 0) return 1;
            }
        } else if (event == 4) {
            return 1;
        }
        if (started && !finished && hakoniwa_simevent_get_state() == 2) {
            finished = scenario == "compare"
                ? run_compare(handle, local_time, work)
                : run_busy(handle, local_time, work);
            if (!finished) return 1;
        }
        if (started && hakoniwa_simevent_get_state() == 2) advance_time(local_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
