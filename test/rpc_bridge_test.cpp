#include "rpc_bridge.hpp"

#include <hako_srv_msgs/pdu_cpptype_conv_SystemControlRequestPacket.hpp>
#include <hako_srv_msgs/pdu_cpptype_conv_SystemControlResponsePacket.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <deque>
#include <utility>

namespace hakoniwa::pdu::rpc {
namespace {

constexpr size_t packet_size = 384;
using RequestHeader = hako::pdu::msgs::hako_srv_msgs::ServiceRequestHeader;
using ResponseHeader = hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader;

template<class Converter, class Header>
PduData packet(Header header, uint8_t body = 0)
{
    PduData pdu(packet_size);
    EXPECT_GT(Converter{}.cpp2pdu(header, reinterpret_cast<char*>(pdu.data()), pdu.size()), 0);
    HakoPduMetaDataType metadata{};
    std::memcpy(&metadata, pdu.data(), sizeof(metadata));
    metadata.heap_off = packet_size - 8;
    metadata.total_size = packet_size;
    std::memcpy(pdu.data(), &metadata, sizeof(metadata));
    pdu.back() = body;
    return pdu;
}

class Server final : public IRpcServerEndpoint {
public:
    Server() : IRpcServerEndpoint("Service/Test", 0) {}

    bool initialize(const nlohmann::json&, int, std::optional<std::string>) override { return true; }
    void clear_pending_requests() override {}
    ServerEventType poll(RpcRequest& request) override
    {
        if (requests.empty()) return ServerEventType::NONE;
        auto [event, incoming] = std::move(requests.front());
        requests.pop_front();
        request = std::move(incoming);
        return event;
    }

    void request(const std::string& name, Hako_uint32 id, bool cancel = false)
    {
        HakoCpp_ServiceRequestHeader header{};
        header.request_id = id;
        header.service_name = service_name_;
        header.client_name = name;
        header.opcode = cancel ? HAKO_SERVICE_OPERATION_CODE_CANCEL : HAKO_SERVICE_OPERATION_CODE_REQUEST;
        header.status_poll_interval_msec = 50;
        requests.emplace_back(cancel ? ServerEventType::REQUEST_CANCEL : ServerEventType::REQUEST_IN,
                              RpcRequest{name, header, packet<RequestHeader>(header, 0x42)});
    }

    void create_reply_buffer(const HakoCpp_ServiceRequestHeader& request, Hako_uint8 status,
                             Hako_int32 result, PduData& pdu) override
    {
        HakoCpp_ServiceResponseHeader header{};
        header.request_id = request.request_id;
        header.client_name = request.client_name;
        header.service_name = request.service_name;
        header.status = status;
        header.result_code = result;
        pdu = packet<ResponseHeader>(header);
    }

    void send_reply(std::string name, const PduData& pdu) override { record(name, pdu, false); }
    void send_cancel_reply(std::string name, const PduData& pdu) override { record(name, pdu, true); }

    struct Reply {
        HakoCpp_ServiceResponseHeader header;
        PduData pdu;
        bool cancel;
    };
    std::deque<std::pair<ServerEventType, RpcRequest>> requests;
    std::vector<Reply> replies;

private:
    void record(const std::string& client, const PduData& pdu, bool cancel)
    {
        HakoCpp_ServiceResponseHeader header{};
        auto copy = pdu;
        EXPECT_TRUE(ResponseHeader{}.pdu2cpp(reinterpret_cast<char*>(copy.data()), header));
        EXPECT_EQ(header.client_name, client);
        replies.push_back({header, pdu, cancel});
    }
};

class Client final : public IRpcClientEndpoint {
public:
    Client() : IRpcClientEndpoint("Service/Test", "BridgeClient", 0) {}

    bool initialize(const nlohmann::json&, int) override { return true; }
    void clear_pending_responses() override {}
    bool call(const PduData& pdu, uint64_t timeout) override
    {
        EXPECT_EQ(timeout, 0u);
        auto copy = pdu;
        HakoCpp_ServiceRequestHeader header{};
        EXPECT_TRUE(RequestHeader{}.pdu2cpp(reinterpret_cast<char*>(copy.data()), header));
        calls.push_back(header);
        sent_packets.push_back(pdu);
        return accepts_calls;
    }

    ClientEventType poll(RpcResponse& response) override
    {
        if (responses.empty()) return ClientEventType::NONE;
        auto [event, incoming] = std::move(responses.front());
        responses.pop_front();
        response = std::move(incoming);
        return event;
    }

    void create_request_buffer(Hako_uint8 opcode, bool cancel, PduData& pdu) override
    {
        EXPECT_FALSE(cancel);
        HakoCpp_ServiceRequestHeader header{};
        header.request_id = ++next_id;
        header.service_name = service_name_;
        header.client_name = client_name_;
        header.opcode = opcode;
        pdu = packet<RequestHeader>(header);
    }

    bool send_cancel_request() override
    {
        ++cancel_attempts;
        return accepts_cancel;
    }

    void reply(Hako_uint32 id, Hako_uint8 status = HAKO_SERVICE_STATUS_DONE,
               Hako_int32 result = HAKO_SERVICE_RESULT_CODE_OK,
               ClientEventType event = ClientEventType::RESPONSE_IN)
    {
        HakoCpp_ServiceResponseHeader header{};
        header.request_id = id;
        header.client_name = client_name_;
        header.service_name = service_name_;
        header.status = status;
        header.result_code = result;
        responses.emplace_back(event, RpcResponse{header, packet<ResponseHeader>(header, 0x84)});
    }

    Hako_uint32 next_id = 100;
    unsigned cancel_attempts = 0;
    bool accepts_cancel = true;
    bool accepts_calls = true;
    std::deque<std::pair<ClientEventType, RpcResponse>> responses;
    std::vector<HakoCpp_ServiceRequestHeader> calls;
    std::vector<PduData> sent_packets;
};

class RpcBridgeTest : public testing::Test {
protected:
    Server server;
    Client client;
    RpcBridge bridge{server, client};
};

TEST_F(RpcBridgeTest, MapsIdentityAndPreservesPacketsInBothDirections)
{
    server.request("Original", 17);
    bridge.poll();
    ASSERT_EQ(client.calls.size(), 1u);
    EXPECT_EQ(client.calls[0].request_id, 101u);
    EXPECT_EQ(client.calls[0].client_name, "BridgeClient");
    EXPECT_EQ(client.calls[0].service_name, "Service/Test");
    EXPECT_EQ(client.calls[0].status_poll_interval_msec, 0u);
    EXPECT_EQ(client.sent_packets[0],
              packet<RequestHeader>(client.calls[0], 0x42));
    client.reply(101);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 1u);
    const auto& reply = server.replies[0];
    EXPECT_EQ(reply.header.request_id, 17u);
    EXPECT_EQ(reply.header.client_name, "Original");
    EXPECT_EQ(reply.pdu, packet<ResponseHeader>(reply.header, 0x84));
    bridge.poll();
    EXPECT_EQ(client.calls.size(), 1u);
    EXPECT_EQ(server.replies.size(), 1u);
}

TEST_F(RpcBridgeTest, PreservesBodyAdjacentToHeaderInBothDirections)
{
    using hako::pdu::msgs::hako_srv_msgs::SystemControlRequestPacket;
    using hako::pdu::msgs::hako_srv_msgs::SystemControlResponsePacket;

    server.request("Original", 17);
    auto& incoming = server.requests.front().second;
    HakoCpp_SystemControlRequestPacket request{};
    request.header = incoming.header;
    request.body.opcode = 1;
    ASSERT_GT(SystemControlRequestPacket{}.cpp2pdu(
        request, reinterpret_cast<char*>(incoming.pdu.data()), incoming.pdu.size()), 0);

    bridge.poll();
    ASSERT_EQ(client.sent_packets.size(), 1u);
    HakoCpp_SystemControlRequestPacket forwarded{};
    ASSERT_TRUE(SystemControlRequestPacket{}.pdu2cpp(
        reinterpret_cast<char*>(client.sent_packets.front().data()), forwarded));
    EXPECT_EQ(forwarded.body.opcode, 1u);

    client.reply(101);
    auto& outgoing = client.responses.front().second;
    HakoCpp_SystemControlResponsePacket response{};
    response.header = outgoing.header;
    response.body.status_code = 1;
    outgoing.pdu.resize(sizeof(Hako_SystemControlResponsePacket) + HAKO_PDU_META_DATA_SIZE());
    ASSERT_GT(SystemControlResponsePacket{}.cpp2pdu(
        response, reinterpret_cast<char*>(outgoing.pdu.data()), outgoing.pdu.size()), 0);

    bridge.poll();
    ASSERT_EQ(server.replies.size(), 1u);
    HakoCpp_SystemControlResponsePacket returned{};
    ASSERT_TRUE(SystemControlResponsePacket{}.pdu2cpp(
        reinterpret_cast<char*>(server.replies.front().pdu.data()), returned));
    EXPECT_EQ(returned.body.status_code, 1u);
}

TEST_F(RpcBridgeTest, RejectsDuplicateAndSerializesDifferentClients)
{
    server.request("First", 17);
    server.request("First", 18);
    server.request("Second", 17);
    bridge.poll();
    bridge.poll();
    bridge.poll();
    ASSERT_EQ(client.calls.size(), 1u);
    ASSERT_EQ(server.replies.size(), 1u);
    EXPECT_EQ(server.replies[0].header.request_id, 18u);
    EXPECT_EQ(server.replies[0].header.result_code, HAKO_SERVICE_RESULT_CODE_BUSY);
    client.reply(101);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 2u);
    EXPECT_EQ(server.replies[1].header.request_id, 17u);
    EXPECT_EQ(server.replies[1].header.client_name, "First");
    ASSERT_EQ(client.calls.size(), 2u);
    client.reply(102);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 3u);
    EXPECT_EQ(server.replies[2].header.request_id, 17u);
    EXPECT_EQ(server.replies[2].header.client_name, "Second");
}

TEST_F(RpcBridgeTest, ForwardsOnlyMatchingCancelAndRoutesCanceledReply)
{
    server.request("Original", 17);
    bridge.poll();
    server.request("Original", 18, true);
    bridge.poll();
    EXPECT_EQ(client.cancel_attempts, 0u);
    client.accepts_cancel = false;
    server.request("Original", 17, true);
    bridge.poll();
    EXPECT_EQ(client.cancel_attempts, 1u);
    client.accepts_cancel = true;
    bridge.poll();
    EXPECT_EQ(client.cancel_attempts, 2u);
    server.request("Original", 17, true);
    bridge.poll();
    EXPECT_EQ(client.cancel_attempts, 2u);
    client.reply(101, HAKO_SERVICE_STATUS_DONE, HAKO_SERVICE_RESULT_CODE_CANCELED,
                 ClientEventType::RESPONSE_CANCEL);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 1u);
    EXPECT_TRUE(server.replies[0].cancel);
    EXPECT_EQ(server.replies[0].header.request_id, 17u);
    EXPECT_EQ(server.replies[0].header.result_code, HAKO_SERVICE_RESULT_CODE_CANCELED);
    server.request("Original", 18);
    bridge.poll();
    EXPECT_EQ(client.calls.size(), 2u);
}

TEST_F(RpcBridgeTest, KeepsMappingAcrossInterimTimeoutAndStaleEvents)
{
    server.request("Original", 17);
    bridge.poll();
    client.reply(101, HAKO_SERVICE_STATUS_DOING);
    client.reply(101, HAKO_SERVICE_STATUS_CANCELING, HAKO_SERVICE_RESULT_CODE_BUSY);
    client.reply(101, HAKO_SERVICE_STATUS_DONE, HAKO_SERVICE_RESULT_CODE_BUSY);
    client.responses.emplace_back(ClientEventType::RESPONSE_TIMEOUT, RpcResponse{});
    client.reply(100);
    for (int i = 0; i < 10; ++i) bridge.poll();
    EXPECT_TRUE(server.replies.empty());
    EXPECT_EQ(client.calls.size(), 1u);
    EXPECT_EQ(client.cancel_attempts, 0u);
    client.reply(101, HAKO_SERVICE_STATUS_ERROR, HAKO_SERVICE_RESULT_CODE_INVALID);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 1u);
    EXPECT_EQ(server.replies[0].header.request_id, 17u);
    EXPECT_EQ(server.replies[0].header.result_code, HAKO_SERVICE_RESULT_CODE_INVALID);
}

TEST_F(RpcBridgeTest, ReportsSendFailureAndAcceptsNextRequest)
{
    client.accepts_calls = false;
    server.request("Original", 17);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 1u);
    EXPECT_EQ(server.replies[0].header.result_code, HAKO_SERVICE_RESULT_CODE_ERROR);
    client.accepts_calls = true;
    server.request("Original", 18);
    bridge.poll();
    ASSERT_EQ(client.calls.size(), 2u);
    client.reply(102);
    bridge.poll();
    ASSERT_EQ(server.replies.size(), 2u);
    EXPECT_EQ(server.replies[1].header.request_id, 18u);
}

} // namespace
} // namespace hakoniwa::pdu::rpc
