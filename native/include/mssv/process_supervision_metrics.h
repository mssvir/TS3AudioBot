#pragma once

#include <atomic>
#include <cstdint>

namespace mssv::process {

struct ProcessSupervisionMetricsSnapshot {
    std::uint64_t active_children{0};
    std::uint64_t started{0};
    std::uint64_t denied_starts{0};
    std::uint64_t start_failures{0};
    std::uint64_t resource_exhaustion_failures{0};
    std::uint64_t missing_executable_failures{0};
    std::uint64_t other_start_failures{0};
    std::uint64_t exits_observed{0};
    std::uint64_t forced_kills{0};
    std::uint64_t cleanup_failures{0};
};

class ProcessSupervisionMetrics final {
public:
    void record_started() noexcept;
    void record_denied_start() noexcept;
    void record_resource_exhaustion_failure() noexcept;
    void record_missing_executable_failure() noexcept;
    void record_other_start_failure() noexcept;
    void record_exit_observed() noexcept;
    void record_forced_kill() noexcept;
    void record_reaped() noexcept;
    void record_cleanup_failure() noexcept;

    [[nodiscard]] ProcessSupervisionMetricsSnapshot snapshot() const noexcept;

private:
    std::atomic<std::uint64_t> active_children_{0};
    std::atomic<std::uint64_t> started_{0};
    std::atomic<std::uint64_t> denied_starts_{0};
    std::atomic<std::uint64_t> start_failures_{0};
    std::atomic<std::uint64_t> resource_exhaustion_failures_{0};
    std::atomic<std::uint64_t> missing_executable_failures_{0};
    std::atomic<std::uint64_t> other_start_failures_{0};
    std::atomic<std::uint64_t> exits_observed_{0};
    std::atomic<std::uint64_t> forced_kills_{0};
    std::atomic<std::uint64_t> cleanup_failures_{0};
};

} // namespace mssv::process
