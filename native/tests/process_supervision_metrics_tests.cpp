#include "mssv/process_supervision_metrics.h"
#include "mssv/process_supervisor.h"

#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using mssv::process::ChildId;
using mssv::process::ProcessBackend;
using mssv::process::ProcessSupervisionMetrics;
using mssv::process::ProcessSupervisor;
using mssv::process::SpawnAdmissionGate;
using mssv::process::SpawnAttempt;
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

    SpawnAttempt spawn() override
    {
        expect(!spawn_results.empty(), "spawn result queue is empty");
        const auto result = spawn_results.front();
        spawn_results.pop_front();
        return result;
    }

    bool request_terminate(ChildId) override
    {
        return terminate_result;
    }

    bool wait_for_exit(ChildId, std::chrono::milliseconds) override
    {
        expect(!wait_results.empty(), "wait result queue is empty");
        const auto result = wait_results.front();
        wait_results.pop_front();
        return result;
    }

    bool force_kill(ChildId) override
    {
        return kill_result;
    }

    bool reap(ChildId) override
    {
        return reap_result;
    }
};

void test_successful_lifecycle_updates_active_and_exit_counters()
{
    SpawnAdmissionGate gate;
    FakeBackend backend;
    backend.spawn_results.push_back({true, 101, 0});
    backend.wait_results.push_back(true);
    ProcessSupervisionMetrics metrics;
    ProcessSupervisor supervisor(backend, gate, true, &metrics);

    (void)supervisor.start();
    auto snapshot = metrics.snapshot();
    expect(snapshot.started == 1, "successful start must increment started");
    expect(snapshot.active_children == 1, "successful start must increment active children");

    const auto stop = supervisor.stop(
        101,
        StopCause::cancellation,
        std::chrono::milliseconds{100},
        std::chrono::milliseconds{100});
    expect(stop.cleanup_complete(), "fixture cleanup must complete");

    snapshot = metrics.snapshot();
    expect(snapshot.active_children == 0, "reap must decrement active children");
    expect(snapshot.exits_observed == 1, "observed exit must increment exit counter");
    expect(snapshot.forced_kills == 0, "graceful stop must not increment forced kill counter");
    expect(snapshot.cleanup_failures == 0, "complete cleanup must not increment cleanup failures");
}

void test_resource_failure_and_admission_denial_are_distinct()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    FakeBackend backend;
    backend.spawn_results.push_back({false, 0, 11});
    ProcessSupervisionMetrics metrics;
    ProcessSupervisor supervisor(backend, gate, true, &metrics);

    (void)supervisor.start();
    (void)supervisor.start();

    const auto snapshot = metrics.snapshot();
    expect(snapshot.start_failures == 1, "resource failure must increment total start failures once");
    expect(snapshot.resource_exhaustion_failures == 1, "EAGAIN must increment resource failure counter");
    expect(snapshot.denied_starts == 1, "cooldown denial must increment denied starts");
    expect(snapshot.started == 0, "failed/denied starts must not increment started");
    expect(snapshot.active_children == 0, "failed/denied starts must not create active children");
}

void test_missing_and_generic_start_failures_are_separated()
{
    SpawnAdmissionGate gate;
    FakeBackend backend;
    backend.spawn_results.push_back({false, 0, 2});
    backend.spawn_results.push_back({false, 0, 13});
    ProcessSupervisionMetrics metrics;
    ProcessSupervisor supervisor(backend, gate, true, &metrics);

    (void)supervisor.start();
    (void)supervisor.start();

    const auto snapshot = metrics.snapshot();
    expect(snapshot.start_failures == 2, "both failed spawns must increment total failures");
    expect(snapshot.missing_executable_failures == 1, "ENOENT must increment missing executable counter");
    expect(snapshot.other_start_failures == 1, "generic native error must increment other failure counter");
    expect(snapshot.resource_exhaustion_failures == 0, "non-resource failures must not increment resource counter");
}

void test_forced_cleanup_failure_stays_visible_and_active()
{
    SpawnAdmissionGate gate;
    FakeBackend backend;
    backend.spawn_results.push_back({true, 202, 0});
    backend.wait_results.push_back(false);
    backend.wait_results.push_back(false);
    backend.reap_result = false;
    ProcessSupervisionMetrics metrics;
    ProcessSupervisor supervisor(backend, gate, true, &metrics);

    (void)supervisor.start();
    const auto stop = supervisor.stop(
        202,
        StopCause::timeout,
        std::chrono::milliseconds{10},
        std::chrono::milliseconds{10});
    expect(!stop.cleanup_complete(), "fixture cleanup must remain incomplete");

    const auto snapshot = metrics.snapshot();
    expect(snapshot.started == 1, "started child must remain counted");
    expect(snapshot.active_children == 1, "unreaped child must remain active");
    expect(snapshot.forced_kills == 1, "successful force-kill request must increment counter");
    expect(snapshot.exits_observed == 0, "unobserved exit must not increment exit counter");
    expect(snapshot.cleanup_failures == 1, "incomplete cleanup must increment cleanup failures");
}

} // namespace

int main()
{
    try {
        test_successful_lifecycle_updates_active_and_exit_counters();
        test_resource_failure_and_admission_denial_are_distinct();
        test_missing_and_generic_start_failures_are_separated();
        test_forced_cleanup_failure_stays_visible_and_active();
        std::cout << "mssv process supervision metrics tests: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv process supervision metrics tests: failed: " << exception.what() << '\n';
        return 1;
    }
}
