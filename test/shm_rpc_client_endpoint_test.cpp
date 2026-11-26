#include "hakoniwa/pdu/rpc/rpc_services_client.hpp"
#include "hakoniwa/pdu/rpc/shm_rpc_client_endpoint.hpp"
#include "pdu_convertor.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

using namespace hakoniwa::pdu::rpc;

namespace
{

class FakeCore final : public CoreProServiceApi
{
public:
    bool start_asset(const std::string&, const std::string&, uint64_t) override
    {
        notify();
        return true;
    }

    int create_client(const std::string&, const std::string&, const std::string&) override
    {
        notify();
        return 0;
    }

    bool call(int, const std::vector<uint8_t>&) override
    {
        calls++;
        {
            std::lock_guard lock(mutex_);
            call_thread_ = std::this_thread::get_id();
        }
        notify();
        return true;
    }

    bool cancel(int) override
    {
        cancels++;
        notify();
        return true;
    }

    CoreProClientEvent poll(int) override
    {
        return event_.exchange(CoreProClientEvent::none);
    }

    bool response(int, std::vector<uint8_t>& out) override
    {
        std::lock_guard lock(mutex_);
        out = reply_;
        return true;
    }

    bool step(uint64_t) override
    {
        steps++;
        notify();
        return true;
    }

    void stop_asset() override
    {
        notify();
    }

    void set_response(CoreProClientEvent event, std::vector<uint8_t> reply)
    {
        {
            std::lock_guard lock(mutex_);
            reply_ = std::move(reply);
        }
        event_ = event;
        notify();
    }

    bool wait_for(const std::function<bool()>& predicate)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(1), predicate);
    }

    std::thread::id call_thread() const
    {
        std::lock_guard lock(mutex_);
        return call_thread_;
    }

    std::atomic<int> calls{0};
    std::atomic<int> cancels{0};
    std::atomic<int> steps{0};

private:
    void notify()
    {
        condition_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<CoreProClientEvent> event_{CoreProClientEvent::none};
    std::thread::id call_thread_{};
    std::vector<uint8_t> reply_;
};

#ifdef HAKO_PDU_RPC_ENABLE_SHM
std::vector<uint8_t> response_pdu(int code, int status = HAKO_SERVICE_STATUS_DONE)
{
    HakoCpp_ServiceResponseHeader header{};
    header.service_name = "svc";
    header.client_name = "client";
    header.status = status;
    header.result_code = code;

    std::vector<uint8_t> pdu(512);
    const auto size = hako::pdu::PduConvertor <
                      HakoCpp_ServiceResponseHeader,
                      hako::pdu::msgs::hako_srv_msgs::ServiceResponseHeader > {}
                      .cpp2pdu(header, reinterpret_cast<char*>(pdu.data()), pdu.size());
    pdu.resize(size);
    return pdu;
}

class ShmEndpointTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fake_ = std::make_shared<FakeCore>();
        asset_ = std::make_shared<ShmRpcAsset>("asset", "config", 1, fake_);
        endpoint_ = std::make_shared<ShmRpcClientEndpoint>("svc", "client", 1, asset_);
        ASSERT_TRUE(endpoint_->initialize(nlohmann::json{{"name", "svc"}}, 0));
    }

    void TearDown() override
    {
        asset_->stop();
    }

    void wait_for(const std::function<bool()>& predicate)
    {
        ASSERT_TRUE(fake_->wait_for(predicate));
    }

    void send_reply(int code, int status = HAKO_SERVICE_STATUS_DONE)
    {
        fake_->set_response(CoreProClientEvent::response_in, response_pdu(code, status));
    }

    std::shared_ptr<FakeCore> fake_;
    std::shared_ptr<ShmRpcAsset> asset_;
    std::shared_ptr<ShmRpcClientEndpoint> endpoint_;
};

TEST_F(ShmEndpointTest, ReplyOk)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    send_reply(HAKO_SERVICE_RESULT_CODE_OK);

    RpcResponse response;
    wait_for([this, &response] {
        return endpoint_->poll(response) == ClientEventType::RESPONSE_IN;
    });
    EXPECT_EQ(response.header.result_code, HAKO_SERVICE_RESULT_CODE_OK);
}

TEST_F(ShmEndpointTest, RefusalInvalid)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    send_reply(HAKO_SERVICE_RESULT_CODE_INVALID);

    RpcResponse response;
    wait_for([this, &response] {
        return endpoint_->poll(response) == ClientEventType::RESPONSE_IN;
    });
    EXPECT_EQ(response.header.result_code, HAKO_SERVICE_RESULT_CODE_INVALID);
}

TEST_F(ShmEndpointTest, CancelGivesCanceled)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    ASSERT_TRUE(endpoint_->send_cancel_request());
    wait_for([this] { return fake_->cancels == 1; });
    fake_->set_response(
        CoreProClientEvent::cancel_done,
        response_pdu(HAKO_SERVICE_RESULT_CODE_CANCELED));

    RpcResponse response;
    wait_for([this, &response] {
        return endpoint_->poll(response) == ClientEventType::RESPONSE_CANCEL;
    });
}

TEST_F(ShmEndpointTest, TimeoutThenCancel)
{
    ASSERT_TRUE(endpoint_->call({1}, 1));
    wait_for([this] { return fake_->calls == 1; });

    RpcResponse response;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (endpoint_->poll(response) != ClientEventType::RESPONSE_TIMEOUT &&
            std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(endpoint_->poll(response), ClientEventType::RESPONSE_TIMEOUT);
    ASSERT_TRUE(endpoint_->send_cancel_request());
    wait_for([this] { return fake_->cancels == 1; });
}

TEST_F(ShmEndpointTest, SecondCallRefusedBeforeCorePro)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    EXPECT_FALSE(endpoint_->call({2}, 0));
    wait_for([this] { return fake_->calls == 1; });
    EXPECT_EQ(fake_->calls, 1);
}

TEST_F(ShmEndpointTest, AssetThreadStepsWhileReplyWithheld)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1 && fake_->steps > 10; });
}

TEST_F(ShmEndpointTest, CoreProCalledOnlyOnAssetThread)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    EXPECT_NE(fake_->call_thread(), std::this_thread::get_id());
}

TEST_F(ShmEndpointTest, BusyNoticeDoesNotFinish)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    send_reply(HAKO_SERVICE_RESULT_CODE_BUSY);
    wait_for([this] { return fake_->steps > 10; });

    RpcResponse response;
    EXPECT_EQ(endpoint_->poll(response), ClientEventType::NONE);
}

TEST_F(ShmEndpointTest, DoingNoticeDoesNotFinish)
{
    ASSERT_TRUE(endpoint_->call({1}, 0));
    wait_for([this] { return fake_->calls == 1; });
    send_reply(HAKO_SERVICE_RESULT_CODE_OK, HAKO_SERVICE_STATUS_DOING);
    wait_for([this] { return fake_->steps > 10; });

    RpcResponse response;
    EXPECT_EQ(endpoint_->poll(response), ClientEventType::NONE);
}
#endif

TEST(ShmDisabledRefusesProtocol, ShmDisabledRefusesProtocol)
{
#ifdef HAKO_PDU_RPC_ENABLE_SHM
    GTEST_SKIP() << "This refusal is specific to an OFF build.";
#else
    auto fake = std::make_shared<FakeCore>();
    auto asset = std::make_shared<ShmRpcAsset>("asset", "config", 1, fake);
    auto endpoint = std::make_shared<ShmRpcClientEndpoint>("svc", "client", 1, asset);
    EXPECT_FALSE(endpoint->initialize(nlohmann::json{{"name", "svc"}}, 0));
#endif
}

class TemporaryConfigDirectory
{
public:
    TemporaryConfigDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("hakoniwa-shm-rpc-" + std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryConfigDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_json(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2);
}

TEST(ShmConfigSelection, SelectsShmEndpointOrRefusesWhenDisabled)
{
    TemporaryConfigDirectory directory;
    write_json(directory.path() / "services.json", {
        {"pduMetaDataSize", 24},
        {
            "services", {{{"name", "svc"},
                    {
                        "clients", {{{"name", "client"},
                                {"client_endpoint", {{"nodeId", "node"}, {"endpointId", "ep"}}}
                            }
                        }
                    }
                }
            }
        },
    });
    write_json(directory.path() / "endpoints.json", {{{"nodeId", "node"}, {
                "endpoints", {{{"id", "ep"}, {"config_path", "endpoint.json"}}}
            }
        }
    });
    write_json(directory.path() / "endpoint.json", {{"comm", "comm.json"}});
    write_json(directory.path() / "comm.json", {
        {"protocol", "shm"},
        {"assetName", "asset"},
        {"coreConfig", "core.json"},
        {"deltaTimeUsec", 1},
    });

    auto fake = std::make_shared<FakeCore>();
    RpcServicesClient client(
        "node",
        "client",
        (directory.path() / "services.json").string(),
        "RpcClientEndpointImpl",
        1,
        "real",
        (directory.path() / "endpoints.json").string(),
        fake);

#ifdef HAKO_PDU_RPC_ENABLE_SHM
    ASSERT_TRUE(client.initialize_services(nullptr));
    ASSERT_TRUE(client.call("svc", {1}, 0));
    ASSERT_TRUE(fake->wait_for([&fake] { return fake->calls == 1; }));
#else
    EXPECT_FALSE(client.initialize_services(nullptr));
#endif
}

} // namespace
