#include <gtest/gtest.h>

#include "hako_action_msgs/pdu_cpptype_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_ActionResponseHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionRequestHeader.hpp"
#include "hako_action_msgs/pdu_cpptype_conv_ActionResponseHeader.hpp"
#include "pdu_convertor.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_FibonacciActionResponse.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionRequest.hpp"
#include "sample_action_msgs/pdu_cpptype_conv_FibonacciActionResponse.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint8_t kRequestKindGoal = 1;
constexpr std::uint8_t kResponseKindGoal = 1;
constexpr std::uint8_t kDecisionAccepted = 1;

const std::array<std::uint8_t, 16> kGoalId{
    0x10, 0x11, 0x12, 0x13,
    0x20, 0x21, 0x22, 0x23,
    0x30, 0x31, 0x32, 0x33,
    0x40, 0x41, 0x42, 0x43,
};

TEST(ActionPacketCodecContract, CommonRequestHeaderDecodesFromGeneratedPacket)
{
    HakoCpp_FibonacciActionRequest source{};
    source.header.version = kProtocolVersion;
    source.header.request_kind = kRequestKindGoal;
    source.header.reserved = {0, 0};
    source.header.goal_id = kGoalId;
    source.body.order = 8;

    hako::pdu::msgs::sample_action_msgs::FibonacciActionRequest packet_convertor;
    std::vector<std::uint8_t> packet(1024, 0);
    const int packet_size = packet_convertor.cpp2pdu(
        source,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    ASSERT_GT(packet_size, 0);
    packet.resize(static_cast<std::size_t>(packet_size));

    hako::pdu::PduConvertor<
        HakoCpp_ActionRequestHeader,
        hako::pdu::msgs::hako_action_msgs::ActionRequestHeader>
        header_convertor;
    HakoCpp_ActionRequestHeader decoded{};
    ASSERT_TRUE(header_convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()), decoded));

    EXPECT_EQ(decoded.version, kProtocolVersion);
    EXPECT_EQ(decoded.request_kind, kRequestKindGoal);
    EXPECT_EQ(decoded.goal_id, kGoalId);
}

TEST(ActionPacketCodecContract, GeneratedResponseKeepsCommonHeaderAndDefaultBody)
{
    HakoCpp_FibonacciActionResponse source{};
    source.header.version = kProtocolVersion;
    source.header.response_kind = kResponseKindGoal;
    source.header.status = kDecisionAccepted;
    source.header.reserved = 0;
    source.header.goal_id = kGoalId;
    source.body.sequence = {};

    hako::pdu::msgs::sample_action_msgs::FibonacciActionResponse packet_convertor;
    std::vector<std::uint8_t> packet(1024, 0);
    const int packet_size = packet_convertor.cpp2pdu(
        source,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()));
    ASSERT_GT(packet_size, 0);
    packet.resize(static_cast<std::size_t>(packet_size));

    hako::pdu::PduConvertor<
        HakoCpp_ActionResponseHeader,
        hako::pdu::msgs::hako_action_msgs::ActionResponseHeader>
        header_convertor;
    HakoCpp_ActionResponseHeader decoded_header{};
    ASSERT_TRUE(header_convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()), decoded_header));

    HakoCpp_FibonacciActionResponse decoded_packet{};
    ASSERT_TRUE(packet_convertor.pdu2cpp(
        reinterpret_cast<char*>(packet.data()), decoded_packet));

    EXPECT_EQ(decoded_header.version, kProtocolVersion);
    EXPECT_EQ(decoded_header.response_kind, kResponseKindGoal);
    EXPECT_EQ(decoded_header.status, kDecisionAccepted);
    EXPECT_EQ(decoded_header.goal_id, kGoalId);
    EXPECT_TRUE(decoded_packet.body.sequence.empty());
}

} // namespace
