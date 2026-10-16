#pragma once

#include <chrono>
#include <functional>
#include <mutex>

namespace mssv::process {

class SpawnAdmissionGate final {
public:
    using Duration = std::chrono::milliseconds;
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;

    explicit SpawnAdmissionGate(
        Clock now = [] { return std::chrono::steady_clock::now(); },
        Duration base_cooldown = Duration{250},
        Duration max_cooldown = Duration{5000},
        Duration probe_retry_delay = Duration{250});

    [[nodiscard]] bool try_enter(Duration& retry_after);
    [[nodiscard]] Duration record_resource_failure();
    void record_success();
    void record_non_resource_failure();

private:
    [[nodiscard]] Duration calculate_cooldown(int failures) const noexcept;

    std::mutex mutex_;
    Clock now_;
    Duration base_cooldown_;
    Duration max_cooldown_;
    Duration probe_retry_delay_;
    int consecutive_resource_failures_{0};
    TimePoint blocked_until_{TimePoint::min()};
    bool probe_in_flight_{false};
};

} // namespace mssv::process
