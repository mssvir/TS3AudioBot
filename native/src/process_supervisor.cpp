#include "mssv/process_supervisor.h"

#include "mssv/process_supervision_metrics.h"

namespace mssv::process {

ProcessSupervisor::ProcessSupervisor(
    ProcessBackend& backend,
    SpawnAdmissionGate& admission_gate,
    const bool is_linux,
    ProcessSupervisionMetrics* const metrics) noexcept
    : backend_(backend), admission_gate_(admission_gate), is_linux_(is_linux), metrics_(metrics)
{
}

StartResult ProcessSupervisor::start()
{
    SpawnAdmissionGate::Duration retry_after{};
    if (!admission_gate_.try_enter(retry_after)) {
        if (metrics_ != nullptr) {
            metrics_->record_denied_start();
        }
        return {
            StartStatus::admission_denied,
            0,
            0,
            retry_after,
        };
    }

    const auto attempt = backend_.spawn();
    if (attempt.started) {
        admission_gate_.record_success();
        if (metrics_ != nullptr) {
            metrics_->record_started();
        }
        return {
            StartStatus::started,
            attempt.child_id,
            0,
            SpawnAdmissionGate::Duration::zero(),
        };
    }

    const auto failure = classify_native_spawn_error(attempt.native_error, is_linux_);
    switch (failure.category) {
    case SpawnFailureCategory::resource_exhaustion: {
        const auto cooldown = admission_gate_.record_resource_failure();
        if (metrics_ != nullptr) {
            metrics_->record_resource_exhaustion_failure();
        }
        return {
            StartStatus::resource_exhaustion,
            0,
            attempt.native_error,
            cooldown,
        };
    }
    case SpawnFailureCategory::missing_executable:
        admission_gate_.record_non_resource_failure();
        if (metrics_ != nullptr) {
            metrics_->record_missing_executable_failure();
        }
        return {
            StartStatus::missing_executable,
            0,
            attempt.native_error,
            SpawnAdmissionGate::Duration::zero(),
        };
    case SpawnFailureCategory::other_native_error:
        admission_gate_.record_non_resource_failure();
        if (metrics_ != nullptr) {
            metrics_->record_other_start_failure();
        }
        return {
            StartStatus::spawn_failed,
            0,
            attempt.native_error,
            SpawnAdmissionGate::Duration::zero(),
        };
    }

    admission_gate_.record_non_resource_failure();
    if (metrics_ != nullptr) {
        metrics_->record_other_start_failure();
    }
    return {
        StartStatus::spawn_failed,
        0,
        attempt.native_error,
        SpawnAdmissionGate::Duration::zero(),
    };
}

StopResult ProcessSupervisor::stop(
    const ChildId child_id,
    const StopCause cause,
    const std::chrono::milliseconds graceful_timeout,
    const std::chrono::milliseconds forced_kill_timeout)
{
    StopResult result{};
    result.cause = cause;
    result.termination_requested = backend_.request_terminate(child_id);

    result.graceful_exit = backend_.wait_for_exit(child_id, graceful_timeout);
    result.exit_observed = result.graceful_exit;

    if (!result.exit_observed) {
        result.forced_kill = backend_.force_kill(child_id);
        if (result.forced_kill) {
            result.exit_observed = backend_.wait_for_exit(child_id, forced_kill_timeout);
        }
    }

    result.reaped = backend_.reap(child_id);

    if (metrics_ != nullptr) {
        if (result.forced_kill) {
            metrics_->record_forced_kill();
        }
        if (result.exit_observed) {
            metrics_->record_exit_observed();
        }
        if (result.reaped) {
            metrics_->record_reaped();
        }
        if (!result.cleanup_complete()) {
            metrics_->record_cleanup_failure();
        }
    }

    return result;
}

} // namespace mssv::process
