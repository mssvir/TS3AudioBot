#include "mssv/linux_process_backend.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

using mssv::process::ChildId;
using mssv::process::LinuxProcessBackend;
using mssv::process::ProcessSpec;
using mssv::process::ProcessSupervisor;
using mssv::process::SpawnAdmissionGate;
using mssv::process::StartStatus;
using mssv::process::StderrCaptureOptions;
using mssv::process::StderrSnapshot;
using mssv::process::StopCause;

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string marker_path(const std::string_view name)
{
    return "/tmp/mssv-" + std::string(name) + "-" + std::to_string(getpid()) + ".pid";
}

pid_t wait_for_marker_pid(const std::string& path, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(path);
        long value{0};
        if (input >> value && value > 0) {
            return static_cast<pid_t>(value);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    throw std::runtime_error("fixture marker was not created before timeout");
}

StderrSnapshot wait_for_complete_stderr(
    const LinuxProcessBackend& backend,
    const ChildId child_id,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto snapshot = backend.stderr_snapshot(child_id);
        expect(snapshot.has_value(), "stderr capture must exist for tracked child");
        if (snapshot->complete) {
            return *snapshot;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("stderr capture did not reach EOF before timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

bool wait_for_process_absent(const pid_t process_id, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        errno = 0;
        if (kill(process_id, 0) != 0 && errno == ESRCH) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

bool wait_for_process_group_absent(const pid_t process_group, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        errno = 0;
        if (kill(-process_group, 0) != 0 && errno == ESRCH) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

void test_real_missing_executable_is_classified()
{
    LinuxProcessBackend backend(ProcessSpec{
        "/mssv/definitely-missing-process-binary",
        {},
        std::nullopt,
    });
    SpawnAdmissionGate gate;
    ProcessSupervisor supervisor(backend, gate, true);

    const auto result = supervisor.start();
    expect(result.status == StartStatus::missing_executable, "real ENOENT must remain missing executable");
    expect(result.native_error == ENOENT, "real ENOENT must be preserved");
    expect(backend.tracked_child_count() == 0, "failed spawn must not create a tracked child");
}

void test_invalid_stderr_capture_is_fail_closed(const std::string& fixture)
{
    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"emit-stderr", "unit-test-secret"},
        StderrCaptureOptions{0, {"unit-test-secret"}},
    });

    const auto attempt = backend.spawn();
    expect(!attempt.started, "invalid zero-byte stderr capture must fail before spawn");
    expect(attempt.native_error == EINVAL, "invalid stderr capture must expose EINVAL");
    expect(backend.tracked_child_count() == 0, "invalid capture must not create child ownership");
}

void test_bounded_stderr_is_drained_and_redacted(const std::string& fixture)
{
    const std::string secret = "unit-test-secret-value";
    constexpr std::size_t capture_limit = 128;
    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"emit-stderr", secret},
        StderrCaptureOptions{capture_limit, {secret}},
    });

    const auto attempt = backend.spawn();
    expect(attempt.started, "stderr fixture must start");
    expect(backend.wait_for_exit(attempt.child_id, std::chrono::seconds{3}), "high-volume stderr fixture must not block on a full pipe");

    const auto snapshot = wait_for_complete_stderr(backend, attempt.child_id, std::chrono::seconds{2});
    expect(snapshot.text.size() <= capture_limit, "exposed stderr must respect the configured byte bound");
    expect(snapshot.truncated, "high-volume stderr must report truncation");
    expect(snapshot.text.find(secret) == std::string::npos, "configured secret literal must never be exposed");
    expect(snapshot.text.find("[redacted]") != std::string::npos, "configured secret literal must be replaced");
    expect(backend.reap(attempt.child_id), "completed stderr fixture must be reaped");
}

void test_truncation_drops_partial_secret_suffix(const std::string& fixture)
{
    const std::string secret = "boundary-secret-value";
    const std::string prefix = "diagnostic-before-";
    const auto capture_limit = prefix.size() + (secret.size() / 2);
    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"emit-stderr", secret},
        StderrCaptureOptions{capture_limit, {secret}},
    });

    const auto attempt = backend.spawn();
    expect(attempt.started, "boundary redaction fixture must start");
    expect(backend.wait_for_exit(attempt.child_id, std::chrono::seconds{3}), "boundary fixture must exit while stderr is drained");

    const auto snapshot = wait_for_complete_stderr(backend, attempt.child_id, std::chrono::seconds{2});
    expect(snapshot.truncated, "boundary fixture must be truncated");
    expect(snapshot.text == prefix, "partial configured secret at capture boundary must be removed");
    expect(backend.reap(attempt.child_id), "boundary fixture must be reaped");
}

void test_graceful_stop_owns_group_and_reaps_process_tree(const std::string& fixture)
{
    const auto marker = marker_path("tree-graceful");
    (void)std::remove(marker.c_str());

    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"tree-graceful", marker},
        std::nullopt,
    });
    SpawnAdmissionGate gate;
    ProcessSupervisor supervisor(backend, gate, true);

    const auto start = supervisor.start();
    expect(start.status == StartStatus::started, "tree fixture must start");
    const auto leader = static_cast<pid_t>(start.child_id);
    expect(getpgid(leader) == leader, "spawned child must lead its own process group");

    const auto descendant = wait_for_marker_pid(marker, std::chrono::seconds{2});
    expect(getpgid(descendant) == leader, "descendant must inherit the supervised process group");

    const auto stop = supervisor.stop(
        start.child_id,
        StopCause::cancellation,
        std::chrono::seconds{2},
        std::chrono::milliseconds{500});

    expect(stop.termination_requested, "graceful tree stop must signal the process group");
    expect(stop.graceful_exit, "graceful tree stop must observe process-group quiescence");
    expect(!stop.forced_kill, "graceful tree stop must not require SIGKILL");
    expect(stop.cleanup_complete(), "graceful tree stop must finish owned-group cleanup");
    expect(backend.tracked_child_count() == 0, "completed cleanup must release tracked ownership");
    expect(wait_for_process_absent(descendant, std::chrono::seconds{2}), "tree descendant must not remain orphaned");
    expect(wait_for_process_group_absent(leader, std::chrono::seconds{2}), "owned process group must disappear after cleanup");

    (void)std::remove(marker.c_str());
}

void test_stubborn_descendant_prevents_false_graceful_completion(const std::string& fixture)
{
    const auto marker = marker_path("tree-stubborn");
    (void)std::remove(marker.c_str());

    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"tree-stubborn", marker},
        std::nullopt,
    });
    SpawnAdmissionGate gate;
    ProcessSupervisor supervisor(backend, gate, true);

    const auto start = supervisor.start();
    expect(start.status == StartStatus::started, "stubborn tree fixture must start");
    const auto leader = static_cast<pid_t>(start.child_id);
    const auto descendant = wait_for_marker_pid(marker, std::chrono::seconds{2});
    expect(getpgid(descendant) == leader, "stubborn descendant must stay in the owned process group");

    const auto stop = supervisor.stop(
        start.child_id,
        StopCause::cancellation,
        std::chrono::milliseconds{75},
        std::chrono::seconds{2});

    expect(stop.termination_requested, "stubborn tree stop must request group SIGTERM first");
    expect(!stop.graceful_exit, "leader exit alone must not count as graceful completion while descendant survives");
    expect(stop.forced_kill, "surviving descendant must force process-group SIGKILL escalation");
    expect(stop.exit_observed, "forced cleanup must observe owned process-group quiescence");
    expect(stop.reaped, "leader ownership must be finalized even if it was reaped during the grace wait");
    expect(stop.cleanup_complete(), "stubborn descendant cleanup must complete after escalation");
    expect(backend.tracked_child_count() == 0, "stubborn cleanup must release tracked ownership");
    expect(wait_for_process_absent(descendant, std::chrono::seconds{2}), "stubborn descendant must not survive cleanup");
    expect(wait_for_process_group_absent(leader, std::chrono::seconds{2}), "stubborn process group must disappear after cleanup");

    (void)std::remove(marker.c_str());
}

void test_timeout_escalates_to_group_sigkill(const std::string& fixture)
{
    const auto marker = marker_path("ignore-term");
    (void)std::remove(marker.c_str());

    LinuxProcessBackend backend(ProcessSpec{
        fixture,
        {"ignore-term", marker},
        std::nullopt,
    });
    SpawnAdmissionGate gate;
    ProcessSupervisor supervisor(backend, gate, true);

    const auto start = supervisor.start();
    expect(start.status == StartStatus::started, "ignore-term fixture must start");
    const auto leader = static_cast<pid_t>(start.child_id);
    expect(getpgid(leader) == leader, "ignore-term child must lead its own process group");
    expect(wait_for_marker_pid(marker, std::chrono::seconds{2}) == leader, "ignore-term fixture must become ready");

    const auto stop = supervisor.stop(
        start.child_id,
        StopCause::timeout,
        std::chrono::milliseconds{50},
        std::chrono::seconds{2});

    expect(stop.termination_requested, "timeout path must request SIGTERM first");
    expect(!stop.graceful_exit, "SIGTERM-ignoring child must exceed the graceful timeout");
    expect(stop.forced_kill, "timeout path must escalate to process-group SIGKILL");
    expect(stop.exit_observed, "SIGKILL path must observe final exit");
    expect(stop.reaped, "SIGKILL path must finalize leader ownership");
    expect(stop.cleanup_complete(), "SIGKILL path must complete cleanup");
    expect(backend.tracked_child_count() == 0, "forced cleanup must release tracked ownership");
    expect(wait_for_process_group_absent(leader, std::chrono::seconds{2}), "forced cleanup process group must disappear");

    (void)std::remove(marker.c_str());
}

} // namespace

int main(const int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: linux_process_backend_integration_tests <fixture-path>\n";
        return 2;
    }

    try {
        const std::string fixture(argv[1]);
        test_real_missing_executable_is_classified();
        test_invalid_stderr_capture_is_fail_closed(fixture);
        test_bounded_stderr_is_drained_and_redacted(fixture);
        test_truncation_drops_partial_secret_suffix(fixture);
        test_graceful_stop_owns_group_and_reaps_process_tree(fixture);
        test_stubborn_descendant_prevents_false_graceful_completion(fixture);
        test_timeout_escalates_to_group_sigkill(fixture);
        std::cout << "mssv Linux process backend integration tests: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv Linux process backend integration tests: failed: " << exception.what() << '\n';
        return 1;
    }
}
