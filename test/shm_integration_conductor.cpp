#include <hakoniwa/hako_conductor.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic_bool keep_running{true};
void stop_handler(int) { keep_running = false; }
}

int main()
{
    std::cout << std::unitbuf;
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    if (hako_conductor_start(1000, 1000) != 0) {
        std::cerr << "CONDUCTOR ERROR: start failed\n";
        return 1;
    }
    std::cout << "CONDUCTOR started\n";
    while (keep_running) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    hako_conductor_stop();
    return 0;
}
