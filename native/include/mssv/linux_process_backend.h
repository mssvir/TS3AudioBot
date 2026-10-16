#pragma once

#include "mssv/process_supervisor.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mssv::process {

inline constexpr std::size_t max_stderr_capture_bytes = 64U * 1024U;
inline constexpr std::size_t max_stderr_redaction_literals = 32U;
inline constexpr std::size_t max_stderr_redaction_literal_bytes = 4096U;

struct StderrCaptureOptions {
    std::size_t max_bytes{16U * 1024U};
    std::vector<std::string> redact_literals;
};

struct StderrSnapshot {
    std::string text;
    bool truncated{false};
    bool complete{false};
};

struct ProcessSpec {
    std::string executable;
    std::vector<std::string> arguments;
    std::optional<StderrCaptureOptions> stderr_capture;
};

class LinuxProcessBackend final : public ProcessBackend {
public:
    explicit LinuxProcessBackend(ProcessSpec spec);
    ~LinuxProcessBackend() override;

    LinuxProcessBackend(const LinuxProcessBackend&) = delete;
    LinuxProcessBackend& operator=(const LinuxProcessBackend&) = delete;
    LinuxProcessBackend(LinuxProcessBackend&&) = delete;
    LinuxProcessBackend& operator=(LinuxProcessBackend&&) = delete;

    [[nodiscard]] SpawnAttempt spawn() override;
    [[nodiscard]] bool request_terminate(ChildId child_id) override;
    [[nodiscard]] bool wait_for_exit(ChildId child_id, std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool force_kill(ChildId child_id) override;
    [[nodiscard]] bool reap(ChildId child_id) override;

    [[nodiscard]] std::size_t tracked_child_count() const noexcept;
    [[nodiscard]] std::optional<StderrSnapshot> stderr_snapshot(ChildId child_id) const;

private:
    struct TrackedChild;

    [[nodiscard]] TrackedChild* find_tracked(ChildId child_id) noexcept;
    [[nodiscard]] const TrackedChild* find_tracked(ChildId child_id) const noexcept;
    [[nodiscard]] bool is_tracked(ChildId child_id) const noexcept;
    [[nodiscard]] bool signal_process_group(ChildId child_id, int signal_number) const noexcept;
    void cleanup_tracked_children() noexcept;

    ProcessSpec spec_;
    std::vector<std::unique_ptr<TrackedChild>> tracked_children_;
};

} // namespace mssv::process
