#include "mssv/process_supervisor.h"

#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using mssv::process::ChildId;
using mssv::process::ProcessBackend;
using mssv::process::ProcessSupervisor;
using mssv::process::SpawnAdmissionGate;
using mssv::process::SpawnAttempt;
using mssv::process::StartStatus;
using mssv::process::StopCause;

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class FakeBackend final : public ProcessBackend {
public:
    std::deque<SpawnAttempt> spawn_results;
    std::deque<bool> wait_results;
    bool terminate_result{true};
    bool kill_result{true};
    bool reap_result{true};
    std::vector<std::string> operations;

    SpawnAttempt spawn() override
    {
        operations.emplace_back("spawn");
        expect(!spawn_results.empty(), "fake backend spawn result queue is empty");
        const auto result = spawn_results.front();
        spawn_results.pop_front();
        return result;
    }

    bool request_terminate(const ChildId child_id) override
    {
        operations.emplace_back("terminate:" + std::to_string(child_id));
        return terminate_result;
    }

    bool wait_for_exit(const ChildId child_id, const std::chrono::milliseconds timeout) override
    {
        operations.emplace_back(
            "wait:" + std::to_string(child_id) + ":" + std::to_string(timeout.count()));
        expect(!wait_results.empty(), "fake backend wait result queue is empty");
        const auto result = wait_results.front();
        wait_results.pop_front();
        return result;
    }

    bool force_kill(const ChildId child_id) override
    {
        operations.emplace_back("kill:" + std::to_string(child_id));
        return kill_result;
    }

    bool reap(const ChildId child_id) override
    {
        operations.emplace_back("reap:" + std::to_string(child_id));
        return reap_result;
    }
};

ProcessSupervisor make_supervisor(
    FakeBackend& backend,
    SpawnAdmissionGate& gate,
    const bool is_linux = true)
{
    return ProcessSupervisor(backend, gate, is_linux);
}

void test_eagain_injection_arms_cooldown_and_denies_retry()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.spawn_results.push_back({false, 0, 11});
    backend.spawn_results.push_back({true, 42, 0});
    auto supervisor = make_supervisor(backend, gate);

    const auto first = supervisor.start();
    expect(first.status == StartStatus::resource_exhaustion, "EAGAIN must be resource exhaustion");
    expect(first.native_error == 11, "EAGAIN native error must be preserved");
    expect(first.retry_after == std::chrono::milliseconds{250}, "EAGAIN must arm first cooldown");

    const auto denied = supervisor.start();
    expect(denied.status == StartStatus::admission_denied, "retry during EAGAIN cooldown must be denied");
    expect(denied.retry_after == std::chrono::milliseconds{250}, "denied retry must expose remaining cooldown");
    expect(backend.operations.size() == 1, "admission denial must not call spawn backend");

    now += first.retry_after;
    const auto recovered = supervisor.start();
    expect(recovered.status == StartStatus::started, "spawn must recover after cooldown");
    expect(recovered.child_id == 42, "recovered child id must be preserved");
    expect(backend.operations.size() == 2, "recovery probe must call backend once");
}

void test_missing_executable_does_not_arm_resource_cooldown()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.spawn_results.push_back({false, 0, 2});
    backend.spawn_results.push_back({true, 7, 0});
    auto supervisor = make_supervisor(backend, gate);

    const auto missing = supervisor.start();
    expect(missing.status == StartStatus::missing_executable, "error 2 must remain missing executable");
    expect(missing.retry_after == std::chrono::milliseconds::zero(), "missing executable must not arm resource cooldown");

    const auto immediate = supervisor.start();
    expect(immediate.status == StartStatus::started, "non-resource failure must not block a later start");
    expect(immediate.child_id == 7, "immediate retry child id must be preserved");
}

void test_generic_spawn_failure_releases_half_open_probe_without_resetting_streak()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.spawn_results.push_back({false, 0, 11});
    backend.spawn_results.push_back({false, 0, 13});
    backend.spawn_results.push_back({true, 99, 0});
    auto supervisor = make_supervisor(backend, gate);

    const auto pressure = supervisor.start();
    now += pressure.retry_after;

    const auto generic = supervisor.start();
    expect(generic.status == StartStatus::spawn_failed, "generic native error must remain a generic spawn failure");

    const auto next_probe = supervisor.start();
    expect(next_probe.status == StartStatus::started, "non-resource failure must release the half-open probe");
    expect(next_probe.child_id == 99, "released recovery probe must preserve child id");
}

void test_cancellation_cleanup_exits_gracefully_without_kill()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.wait_results.push_back(true);
    auto supervisor = make_supervisor(backend, gate);

    const auto result = supervisor.stop(
        101,
        StopCause::cancellation,
        std::chrono::milliseconds{750},
        std::chrono::milliseconds{250});

    expect(result.cause == StopCause::cancellation, "stop cause must remain cancellation");
    expect(result.termination_requested, "cancellation must request termination");
    expect(result.graceful_exit, "cancellation must observe graceful exit when backend reports it");
    expect(!result.forced_kill, "graceful cancellation must not force kill");
    expect(result.exit_observed, "graceful cancellation must observe exit");
    expect(result.reaped, "graceful cancellation must reap child");
    expect(result.cleanup_complete(), "graceful cancellation cleanup must be complete");

    const std::vector<std::string> expected{
        "terminate:101",
        "wait:101:750",
        "reap:101",
    };
    expect(backend.operations == expected, "graceful cancellation operation order changed");
}

void test_timeout_escalates_to_force_kill_then_reaps()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.wait_results.push_back(false);
    backend.wait_results.push_back(true);
    auto supervisor = make_supervisor(backend, gate);

    const auto result = supervisor.stop(
        202,
        StopCause::timeout,
        std::chrono::milliseconds{500},
        std::chrono::milliseconds{200});

    expect(result.cause == StopCause::timeout, "stop cause must remain timeout");
    expect(result.termination_requested, "timeout cleanup must request termination first");
    expect(!result.graceful_exit, "first wait must record timeout");
    expect(result.forced_kill, "timeout cleanup must escalate to force kill");
    expect(result.exit_observed, "forced-kill wait must observe final exit");
    expect(result.reaped, "timeout cleanup must reap child");
    expect(result.cleanup_complete(), "timeout cleanup must complete after forced kill");

    const std::vector<std::string> expected{
        "terminate:202",
        "wait:202:500",
        "kill:202",
        "wait:202:200",
        "reap:202",
    };
    expect(backend.operations == expected, "timeout escalation operation order changed");
}

void test_cleanup_failure_is_visible_and_not_reported_complete()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.wait_results.push_back(false);
    backend.wait_results.push_back(false);
    backend.reap_result = false;
    auto supervisor = make_supervisor(backend, gate);

    const auto result = supervisor.stop(
        303,
        StopCause::timeout,
        std::chrono::milliseconds{100},
        std::chrono::milliseconds{100});

    expect(result.forced_kill, "cleanup must attempt force kill after graceful wait expires");
    expect(!result.exit_observed, "failed forced-kill wait must stay visible");
    expect(!result.reaped, "reap failure must stay visible");
    expect(!result.cleanup_complete(), "failed cleanup must never be reported complete");
}

void test_failed_terminate_still_waits_and_escalates_deterministically()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.terminate_result = false;
    backend.wait_results.push_back(false);
    backend.wait_results.push_back(true);
    auto supervisor = make_supervisor(backend, gate);

    const auto result = supervisor.stop(
        404,
        StopCause::cancellation,
        std::chrono::milliseconds{300},
        std::chrono::milliseconds{150});

    expect(!result.termination_requested, "termination request failure must stay visible");
    expect(result.forced_kill, "failed terminate must still escalate after no exit is observed");
    expect(result.exit_observed, "forced kill must still be observed");
    expect(result.reaped, "forced cleanup must still reap child");
    expect(result.cleanup_complete(), "successful escalation must complete cleanup");
}

} // namespace

int main()
{
    try {
        test_eagain_injection_arms_cooldown_and_denies_retry();
        test_missing_executable_does_not_arm_resource_cooldown();
        test_generic_spawn_failure_releases_half_open_probe_without_resetting_streak();
        test_cancellation_cleanup_exits_gracefully_without_kill();
        test_timeout_escalates_to_force_kill_then_reaps();
        test_cleanup_failure_is_visible_and_not_reported_complete();
        test_failed_terminate_still_waits_and_escalates_deterministically();
        std::cout << "mssv process supervisor failure injection tests: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv process supervisor failure injection tests: failed: " << exception.what() << '\n';
        return 1;
    }
}
