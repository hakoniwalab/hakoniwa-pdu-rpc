#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

namespace hakoniwa::pdu::rpc {

/**
 * @brief Abstract interface for a time source.
 * This allows swapping between real-time and simulated time.
 */
class ITimeSource {
public:
    virtual ~ITimeSource() = default;

    /**
     * @brief Gets the current time in microseconds since an arbitrary epoch.
     * The epoch should be consistent for the lifetime of the application.
     * @return The current time in microseconds.
     */
    virtual uint64_t get_current_time_usec() const = 0;

    virtual void sleep(uint64_t time_usec) = 0;
};

/**
 * @brief A time source based on the system's steady (monotonic) clock.
 */
class RealTimeSource : public ITimeSource {
public:
    uint64_t get_current_time_usec() const override {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }
    void sleep(uint64_t time_usec) override {
        std::this_thread::sleep_for(std::chrono::microseconds(time_usec));
    }
};

} // namespace hakoniwa::pdu::rpc
