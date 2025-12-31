#include <gtest/gtest.h>
#include "hakoniwa/pdu/hakoniwa_pdu_rpc.hpp"

TEST(HakoniwaPduRpcTest, Init) {
    hakoniwa::pdu::hako_pdu_rpc_init();
    SUCCEED();
}
