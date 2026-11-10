#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace ILLIXR::diag {

/**
 * Calls a reporting function periodically from its own detached thread, so
 * that a stalled pipeline thread cannot stop the report. Diagnostics only.
 */
class periodic_reporter {
public:
    periodic_reporter()                                    = default;
    periodic_reporter(const periodic_reporter&)            = delete;
    periodic_reporter& operator=(const periodic_reporter&) = delete;

    ~periodic_reporter() {
        stop();
    }

    void start(std::chrono::milliseconds period, std::function<void()> report) {
        std::shared_ptr<std::atomic<bool>> stopped = stopped_;
        std::thread([stopped, period, report = std::move(report)]() {
            while (!stopped->load()) {
                auto deadline = std::chrono::steady_clock::now() + period;
                while (!stopped->load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (stopped->load()) {
                    break;
                }
                report();
            }
        }).detach();
    }

    void stop() {
        stopped_->store(true);
    }

private:
    std::shared_ptr<std::atomic<bool>> stopped_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace ILLIXR::diag
