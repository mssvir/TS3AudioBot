#pragma once

#include "mssv/process_supervisor.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace mssv::process {

struct ProcessSpec {
    std::string executable;
    std::vector<std::string> arguments;
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

private:
    [[nodiscard]] bool is_tracked(ChildId child_id) const noexcept;
    [[nodiscard]] bool signal_process_group(ChildId child_id, int signal_number) const noexcept;
    void cleanup_tracked_children() noexcept;

    ProcessSpec spec_;
    std::unordered_set<ChildId> tracked_children_;
};

} // namespace mssv::process
