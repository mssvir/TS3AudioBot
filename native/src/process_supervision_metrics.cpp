#include "mssv/process_supervision_metrics.h"

#include <atomic>

namespace mssv::process {

void ProcessSupervisionMetrics::record_started() noexcept
{
    started_.fetch_add(1, std::memory_order_relaxed);
    active_children_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_denied_start() noexcept
{
    denied_starts_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_resource_exhaustion_failure() noexcept
{
    start_failures_.fetch_add(1, std::memory_order_relaxed);
    resource_exhaustion_failures_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_missing_executable_failure() noexcept
{
    start_failures_.fetch_add(1, std::memory_order_relaxed);
    missing_executable_failures_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_other_start_failure() noexcept
{
    start_failures_.fetch_add(1, std::memory_order_relaxed);
    other_start_failures_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_exit_observed() noexcept
{
    exits_observed_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_forced_kill() noexcept
{
    forced_kills_.fetch_add(1, std::memory_order_relaxed);
}

void ProcessSupervisionMetrics::record_reaped() noexcept
{
    auto current = active_children_.load(std::memory_order_relaxed);
    while (current != 0
        && !active_children_.compare_exchange_weak(
            current,
            current - 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
    }
}

void ProcessSupervisionMetrics::record_cleanup_failure() noexcept
{
    cleanup_failures_.fetch_add(1, std::memory_order_relaxed);
}

ProcessSupervisionMetricsSnapshot ProcessSupervisionMetrics::snapshot() const noexcept
{
    return {
        active_children_.load(std::memory_order_relaxed),
        started_.load(std::memory_order_relaxed),
        denied_starts_.load(std::memory_order_relaxed),
        start_failures_.load(std::memory_order_relaxed),
        resource_exhaustion_failures_.load(std::memory_order_relaxed),
        missing_executable_failures_.load(std::memory_order_relaxed),
        other_start_failures_.load(std::memory_order_relaxed),
        exits_observed_.load(std::memory_order_relaxed),
        forced_kills_.load(std::memory_order_relaxed),
        cleanup_failures_.load(std::memory_order_relaxed),
    };
}

} // namespace mssv::process
