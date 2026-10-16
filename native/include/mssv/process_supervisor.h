#pragma once

#include "mssv/process_spawn_error.h"
#include "mssv/spawn_admission_gate.h"

#include <chrono>
#include <cstdint>

namespace mssv::process {

using ChildId = std::uint64_t;

struct SpawnAttempt {
    bool started{false};
    ChildId child_id{0};
    int native_error{0};
};

class ProcessBackend {
public:
    virtual ~ProcessBackend() = default;

    [[nodiscard]] virtual SpawnAttempt spawn() = 0;
    [[nodiscard]] virtual bool request_terminate(ChildId child_id) = 0;
    [[nodiscard]] virtual bool wait_for_exit(ChildId child_id, std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual bool force_kill(ChildId child_id) = 0;
    [[nodiscard]] virtual bool reap(ChildId child_id) = 0;
};

enum class StartStatus {
    started,
    admission_denied,
    resource_exhaustion,
    missing_executable,
    spawn_failed,
};

struct StartResult {
    StartStatus status{StartStatus::spawn_failed};
    ChildId child_id{0};
    int native_error{0};
    std::chrono::milliseconds retry_after{0};
};

enum class StopCause {
    cancellation,
    timeout,
};

struct StopResult {
    StopCause cause{StopCause::cancellation};
    bool termination_requested{false};
    bool graceful_exit{false};
    bool forced_kill{false};
    bool exit_observed{false};
    bool reaped{false};

    [[nodiscard]] bool cleanup_complete() const noexcept
    {
        return exit_observed && reaped;
    }
};

class ProcessSupervisor final {
public:
    ProcessSupervisor(ProcessBackend& backend, SpawnAdmissionGate& admission_gate, bool is_linux) noexcept;

    [[nodiscard]] StartResult start();
    [[nodiscard]] StopResult stop(
        ChildId child_id,
        StopCause cause,
        std::chrono::milliseconds graceful_timeout,
        std::chrono::milliseconds forced_kill_timeout);

private:
    ProcessBackend& backend_;
    SpawnAdmissionGate& admission_gate_;
    bool is_linux_;
};

} // namespace mssv::process
