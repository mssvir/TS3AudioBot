#include "mssv/linux_process_backend.h"

#if !defined(__linux__)
#error "LinuxProcessBackend is only supported on Linux"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <limits>
#include <new>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace mssv::process {
namespace {

constexpr auto wait_poll_interval = std::chrono::milliseconds{5};

bool contains_embedded_null(const std::string& value) noexcept
{
    return value.find('\0') != std::string::npos;
}

} // namespace

LinuxProcessBackend::LinuxProcessBackend(ProcessSpec spec)
    : spec_(std::move(spec))
{
}

LinuxProcessBackend::~LinuxProcessBackend()
{
    cleanup_tracked_children();
}

SpawnAttempt LinuxProcessBackend::spawn()
{
    if (spec_.executable.empty() || contains_embedded_null(spec_.executable)) {
        return {false, 0, ENOENT};
    }

    for (const auto& argument : spec_.arguments) {
        if (contains_embedded_null(argument)) {
            return {false, 0, EINVAL};
        }
    }

    try {
        tracked_children_.reserve(tracked_children_.size() + 1);
    } catch (const std::bad_alloc&) {
        return {false, 0, ENOMEM};
    }

    std::vector<std::string> command;
    command.reserve(spec_.arguments.size() + 1);
    command.push_back(spec_.executable);
    command.insert(command.end(), spec_.arguments.begin(), spec_.arguments.end());

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& element : command) {
        argv.push_back(element.data());
    }
    argv.push_back(nullptr);

    posix_spawnattr_t attributes{};
    auto native_error = posix_spawnattr_init(&attributes);
    if (native_error != 0) {
        return {false, 0, native_error};
    }

    native_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (native_error == 0) {
        native_error = posix_spawnattr_setpgroup(&attributes, 0);
    }

    pid_t process_id{-1};
    if (native_error == 0) {
        native_error = posix_spawn(
            &process_id,
            spec_.executable.c_str(),
            nullptr,
            &attributes,
            argv.data(),
            environ);
    }

    (void)posix_spawnattr_destroy(&attributes);

    if (native_error != 0 || process_id <= 0) {
        return {false, 0, native_error != 0 ? native_error : ECHILD};
    }

    const auto child_id = static_cast<ChildId>(process_id);
    tracked_children_.push_back(child_id);
    return {true, child_id, 0};
}

bool LinuxProcessBackend::request_terminate(const ChildId child_id)
{
    return signal_process_group(child_id, SIGTERM);
}

bool LinuxProcessBackend::wait_for_exit(
    const ChildId child_id,
    const std::chrono::milliseconds timeout)
{
    if (!is_tracked(child_id) || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    const auto process_id = static_cast<pid_t>(child_id);
    const auto bounded_timeout = timeout < std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds::zero()
        : timeout;
    const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;

    while (true) {
        siginfo_t info{};
        const auto wait_result = waitid(
            P_PID,
            static_cast<id_t>(process_id),
            &info,
            WEXITED | WNOHANG | WNOWAIT);

        if (wait_result == 0 && info.si_pid == process_id) {
            return true;
        }
        if (wait_result != 0 && errno != EINTR) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(remaining < wait_poll_interval ? remaining : wait_poll_interval);
    }
}

bool LinuxProcessBackend::force_kill(const ChildId child_id)
{
    return signal_process_group(child_id, SIGKILL);
}

bool LinuxProcessBackend::reap(const ChildId child_id)
{
    const auto tracked = std::find(tracked_children_.begin(), tracked_children_.end(), child_id);
    if (tracked == tracked_children_.end()
        || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    const auto process_id = static_cast<pid_t>(child_id);
    int status{0};

    while (true) {
        const auto wait_result = waitpid(process_id, &status, WNOHANG);
        if (wait_result == process_id) {
            tracked_children_.erase(tracked);
            return true;
        }
        if (wait_result == 0) {
            return false;
        }
        if (wait_result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

std::size_t LinuxProcessBackend::tracked_child_count() const noexcept
{
    return tracked_children_.size();
}

bool LinuxProcessBackend::is_tracked(const ChildId child_id) const noexcept
{
    return std::find(tracked_children_.begin(), tracked_children_.end(), child_id)
        != tracked_children_.end();
}

bool LinuxProcessBackend::signal_process_group(
    const ChildId child_id,
    const int signal_number) const noexcept
{
    if (!is_tracked(child_id)
        || child_id == 0
        || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    const auto process_id = static_cast<pid_t>(child_id);
    return kill(-process_id, signal_number) == 0;
}

void LinuxProcessBackend::cleanup_tracked_children() noexcept
{
    for (const auto child_id : tracked_children_) {
        if (child_id == 0 || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
            continue;
        }
        const auto process_id = static_cast<pid_t>(child_id);
        (void)kill(-process_id, SIGKILL);
    }

    for (const auto child_id : tracked_children_) {
        if (child_id == 0 || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
            continue;
        }
        const auto process_id = static_cast<pid_t>(child_id);
        int status{0};
        while (waitpid(process_id, &status, 0) < 0 && errno == EINTR) {
        }
    }

    tracked_children_.clear();
}

} // namespace mssv::process
