#include "mssv/process_spawn_error.h"
#include "mssv/spawn_admission_gate.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using mssv::process::SpawnAdmissionGate;
using mssv::process::SpawnFailureCategory;
using mssv::process::classify_native_spawn_error;

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_spawn_error_classification()
{
    const auto missing = classify_native_spawn_error(2, true);
    expect(missing.category == SpawnFailureCategory::missing_executable,
           "native error 2 must remain the missing executable case");

    const auto eagain = classify_native_spawn_error(11, true);
    expect(eagain.category == SpawnFailureCategory::resource_exhaustion,
           "Linux errno 11 must remain resource exhaustion");

    const auto non_linux_eagain = classify_native_spawn_error(11, false);
    expect(non_linux_eagain.category == SpawnFailureCategory::other_native_error,
           "errno 11 must not be treated as Linux EAGAIN on other platforms");

    const auto other = classify_native_spawn_error(13, true);
    expect(other.category == SpawnFailureCategory::other_native_error,
           "generic native errors must stay distinct from missing executable");
}

void test_fresh_gate_allows_starts()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    SpawnAdmissionGate::Duration retry{};

    expect(gate.try_enter(retry), "fresh gate must allow first start");
    expect(retry == SpawnAdmissionGate::Duration::zero(), "fresh gate retry must be zero");
    expect(gate.try_enter(retry), "fresh gate must allow concurrent starts before resource pressure");
}

void test_resource_failure_blocks_then_allows_single_probe()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    SpawnAdmissionGate::Duration retry{};

    expect(gate.try_enter(retry), "initial start must be admitted");
    const auto cooldown = gate.record_resource_failure();
    expect(cooldown == std::chrono::milliseconds{250}, "first cooldown must be 250ms");
    expect(!gate.try_enter(retry), "gate must block during cooldown");
    expect(retry == std::chrono::milliseconds{250}, "retry delay must match remaining cooldown");

    now += cooldown;
    expect(gate.try_enter(retry), "one half-open recovery probe must be admitted");
    expect(!gate.try_enter(retry), "competing half-open probe must be rejected");
    expect(retry == std::chrono::milliseconds{250}, "competing probe retry must use probe delay");
}

void test_exponential_cooldown_is_bounded()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    SpawnAdmissionGate::Duration retry{};
    constexpr int expected_ms[] = {250, 500, 1000, 2000, 4000, 5000, 5000};

    for (const auto expected : expected_ms) {
        const auto cooldown = gate.record_resource_failure();
        expect(cooldown == std::chrono::milliseconds{expected}, "cooldown sequence diverged from managed contract");
        now += cooldown;
        expect(gate.try_enter(retry), "half-open probe must be available after cooldown");
    }
}

void test_success_resets_resource_pressure_state()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    SpawnAdmissionGate::Duration retry{};

    const auto cooldown = gate.record_resource_failure();
    now += cooldown;
    expect(gate.try_enter(retry), "recovery probe must be admitted");

    gate.record_success();
    expect(gate.try_enter(retry), "success must reset gate");
    expect(gate.try_enter(retry), "reset gate must again allow normal concurrent starts");
}

void test_non_resource_failure_releases_probe_without_resetting_streak()
{
    auto now = SpawnAdmissionGate::TimePoint{};
    SpawnAdmissionGate gate([&now] { return now; });
    SpawnAdmissionGate::Duration retry{};

    const auto cooldown = gate.record_resource_failure();
    now += cooldown;
    expect(gate.try_enter(retry), "half-open probe must be admitted");

    gate.record_non_resource_failure();
    expect(gate.try_enter(retry), "non-resource failure must release the half-open probe");
    expect(!gate.try_enter(retry), "resource-pressure streak must remain until a real success");
}

} // namespace

int main()
{
    try {
        test_spawn_error_classification();
        test_fresh_gate_allows_starts();
        test_resource_failure_blocks_then_allows_single_probe();
        test_exponential_cooldown_is_bounded();
        test_success_resets_resource_pressure_state();
        test_non_resource_failure_releases_probe_without_resetting_streak();
        std::cout << "mssv native process supervision tests: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv native process supervision tests: failed: " << exception.what() << '\n';
        return 1;
    }
}
