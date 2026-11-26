#include "hakoniwa/pdu/rpc/c_rpc.h"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsRequestPacket.hpp"
#include "hako_srv_msgs/pdu_cpptype_conv_AddTwoIntsResponsePacket.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsRequestPacket;
using hako::pdu::msgs::hako_srv_msgs::AddTwoIntsResponsePacket;

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    constexpr auto service = "Service/Add";
    const auto destroy = hako_pdu_rpc_client_destroy;
    std::unique_ptr<hako_pdu_rpc_client_handle_t, decltype(destroy)> client(
        hako_pdu_rpc_client_create("shm_client_node", "ShmClient", argv[1], nullptr, 1000, "real"),
        destroy);
    if (!client) {
        std::cerr << "C API SHM client creation failed with a null endpoint config path" << std::endl;
        return 1;
    }
    if (hako_pdu_rpc_client_start(client.get()) != HAKO_PDU_RPC_OK) return 1;
    std::cout << "CLIENT registered" << std::endl;

    const auto start = std::filesystem::path(argv[4]) / "start-go";
    const auto start_deadline = std::chrono::steady_clock::now() + 10s;
    while (!std::filesystem::exists(start)) {
        if (std::chrono::steady_clock::now() >= start_deadline) return 1;
        std::this_thread::sleep_for(1ms);
    }

    size_t size = 0;
    if (hako_pdu_rpc_client_create_request_buffer(client.get(), service, nullptr, 0, &size)
        != HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL) return 1;
    std::vector<uint8_t> request(size);
    if (hako_pdu_rpc_client_create_request_buffer(client.get(), service, request.data(), request.size(), &size)
        != HAKO_PDU_RPC_OK) return 1;
    AddTwoIntsRequestPacket request_converter;
    HakoCpp_AddTwoIntsRequestPacket packet{};
    auto* bytes = reinterpret_cast<char*>(request.data());
    if (!request_converter.pdu2cpp(bytes, packet)) return 1;
    packet.body.a = 19;
    packet.body.b = 23;
    const int encoded_size = request_converter.cpp2pdu(packet, bytes, request.size());
    if (encoded_size <= 0 || hako_pdu_rpc_client_call(
        client.get(), service, request.data(), encoded_size, 0) != HAKO_PDU_RPC_OK) return 1;

    std::vector<uint8_t> reply(1024);
    hako_pdu_rpc_response_info_t info{};
    hako_pdu_rpc_error_t error{};
    const auto reply_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < reply_deadline) {
        const auto event = hako_pdu_rpc_client_poll(
            client.get(), &info, reply.data(), reply.size(), &size, &error);
        if (error != HAKO_PDU_RPC_OK) return 1;
        if (event == HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN) {
            HakoCpp_AddTwoIntsResponsePacket response{};
            if (std::strcmp(info.service_name, service) != 0 ||
                !AddTwoIntsResponsePacket{}.pdu2cpp(reinterpret_cast<char*>(reply.data()), response) ||
                response.body.sum != 42) return 1;
            if (hako_pdu_rpc_client_stop(client.get()) != HAKO_PDU_RPC_OK) return 1;
            client.reset();
            std::cout << "PASS shm round trip sum=42" << std::endl;
            return 0;
        }
        std::this_thread::sleep_for(1ms);
    }
    return 1;
}
