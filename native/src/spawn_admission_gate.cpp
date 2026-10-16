#include "mssv/spawn_admission_gate.h"

#include <algorithm>
#include <stdexcept>

namespace mssv::process {

SpawnAdmissionGate::SpawnAdmissionGate(
    Clock now,
    const Duration base_cooldown,
    const Duration max_cooldown,
    const Duration probe_retry_delay)
    : now_(std::move(now)),
      base_cooldown_(base_cooldown),
      max_cooldown_(max_cooldown),
      probe_retry_delay_(probe_retry_delay)
{
    if (!now_) {
        throw std::invalid_argument("clock must be callable");
    }
    if (base_cooldown_ <= Duration::zero()) {
        throw std::invalid_argument("base cooldown must be positive");
    }
    if (max_cooldown_ < base_cooldown_) {
        throw std::invalid_argument("max cooldown must be >= base cooldown");
    }
    if (probe_retry_delay_ <= Duration::zero()) {
        throw std::invalid_argument("probe retry delay must be positive");
    }
}

bool SpawnAdmissionGate::try_enter(Duration& retry_after)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = now_();

    if (now < blocked_until_) {
        retry_after = std::chrono::duration_cast<Duration>(blocked_until_ - now);
        return false;
    }

    if (consecutive_resource_failures_ > 0) {
        if (probe_in_flight_) {
            retry_after = probe_retry_delay_;
            return false;
        }
        probe_in_flight_ = true;
    }

    retry_after = Duration::zero();
    return true;
}

SpawnAdmissionGate::Duration SpawnAdmissionGate::record_resource_failure()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (consecutive_resource_failures_ < 30) {
        ++consecutive_resource_failures_;
    }

    const auto cooldown = calculate_cooldown(consecutive_resource_failures_);
    blocked_until_ = now_() + cooldown;
    probe_in_flight_ = false;
    return cooldown;
}

void SpawnAdmissionGate::record_success()
{
    std::lock_guard<std::mutex> lock(mutex_);
    consecutive_resource_failures_ = 0;
    blocked_until_ = TimePoint::min();
    probe_in_flight_ = false;
}

void SpawnAdmissionGate::record_non_resource_failure()
{
    std::lock_guard<std::mutex> lock(mutex_);
    probe_in_flight_ = false;
}

SpawnAdmissionGate::Duration SpawnAdmissionGate::calculate_cooldown(const int failures) const noexcept
{
    auto value = base_cooldown_.count();
    const auto maximum = max_cooldown_.count();

    for (int index = 1; index < failures && value < maximum; ++index) {
        if (value >= maximum / 2) {
            value = maximum;
            break;
        }
        value *= 2;
    }

    return Duration{std::min(value, maximum)};
}

} // namespace mssv::process
