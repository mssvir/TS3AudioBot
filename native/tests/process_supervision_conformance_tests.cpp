#include "mssv/process_spawn_error.h"
#include "mssv/spawn_admission_gate.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using mssv::process::SpawnAdmissionGate;
using mssv::process::SpawnFailureCategory;
using mssv::process::classify_native_spawn_error;

struct ClassificationCase {
    std::string name;
    int native_error;
    std::string platform;
    SpawnFailureCategory expected;
};

struct Fixture {
    std::vector<ClassificationCase> classifications;
    std::unordered_map<std::string, std::string> policies;
};

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::string> split(const std::string& value, const char delimiter)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(delimiter, begin);
        parts.emplace_back(value.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

SpawnFailureCategory parse_category(const std::string& value)
{
    if (value == "missing_executable") {
        return SpawnFailureCategory::missing_executable;
    }
    if (value == "resource_exhaustion") {
        return SpawnFailureCategory::resource_exhaustion;
    }
    if (value == "other_native_error") {
        return SpawnFailureCategory::other_native_error;
    }
    throw std::runtime_error("unknown fixture category: " + value);
}

Fixture load_fixture()
{
    std::ifstream input("process_supervision_v1.tsv");
    expect(input.is_open(), "shared conformance fixture was not copied into the native test directory");

    Fixture fixture;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto fields = split(line, '\t');
        expect(!fields.empty(), "fixture row must not be empty");
        if (fields[0] == "classification") {
            expect(fields.size() == 5, "classification fixture row must have five fields");
            fixture.classifications.push_back({
                fields[1],
                std::stoi(fields[2]),
                fields[3],
                parse_category(fields[4]),
            });
        } else if (fields[0] == "policy") {
            expect(fields.size() == 3, "policy fixture row must have three fields");
            fixture.policies.emplace(fields[1], fields[2]);
        } else {
            throw std::runtime_error("unknown fixture row kind: " + fields[0]);
        }
    }

    expect(!fixture.classifications.empty(), "fixture must define classification cases");
    return fixture;
}

const std::string& policy(const Fixture& fixture, const std::string& name)
{
    const auto found = fixture.policies.find(name);
    if (found == fixture.policies.end()) {
        throw std::runtime_error("missing policy in fixture: " + name);
    }
    return found->second;
}

bool parse_bool(const std::string& value)
{
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean fixture value: " + value);
}

std::vector<int> parse_int_list(const std::string& value)
{
    std::vector<int> result;
    for (const auto& item : split(value, ',')) {
        result.push_back(std::stoi(item));
    }
    return result;
}

SpawnAdmissionGate make_gate(const Fixture& fixture, SpawnAdmissionGate::TimePoint& now)
{
    return SpawnAdmissionGate(
        [&now] { return now; },
        std::chrono::milliseconds{std::stoi(policy(fixture, "base_cooldown_ms"))},
        std::chrono::milliseconds{std::stoi(policy(fixture, "max_cooldown_ms"))},
        std::chrono::milliseconds{std::stoi(policy(fixture, "probe_retry_ms"))});
}

void test_classification_fixture(const Fixture& fixture)
{
    for (const auto& item : fixture.classifications) {
        const bool is_linux = item.platform == "linux";
        expect(item.platform == "linux" || item.platform == "any", "unsupported fixture platform");
        const auto actual = classify_native_spawn_error(item.native_error, is_linux);
        expect(actual.category == item.expected, "native classification diverged from shared fixture: " + item.name);
    }
}

void test_gate_fixture(const Fixture& fixture)
{
    SpawnAdmissionGate::Duration retry{};

    {
        auto now = SpawnAdmissionGate::TimePoint{};
        auto gate = make_gate(fixture, now);
        const auto fresh_starts = std::stoi(policy(fixture, "fresh_concurrent_starts"));
        for (int index = 0; index < fresh_starts; ++index) {
            expect(gate.try_enter(retry), "fresh admission diverged from shared fixture");
            expect(retry == SpawnAdmissionGate::Duration::zero(), "fresh retry must be zero");
        }
    }

    {
        auto now = SpawnAdmissionGate::TimePoint{};
        auto gate = make_gate(fixture, now);
        for (const auto expected_ms : parse_int_list(policy(fixture, "cooldown_sequence_ms"))) {
            const auto cooldown = gate.record_resource_failure();
            expect(cooldown == std::chrono::milliseconds{expected_ms}, "cooldown sequence diverged from shared fixture");
            now += cooldown;
            expect(gate.try_enter(retry), "half-open probe must be admitted after fixture cooldown");
        }
    }

    if (parse_bool(policy(fixture, "single_half_open_probe"))) {
        auto now = SpawnAdmissionGate::TimePoint{};
        auto gate = make_gate(fixture, now);
        const auto cooldown = gate.record_resource_failure();
        now += cooldown;
        expect(gate.try_enter(retry), "first half-open probe must be admitted");
        expect(!gate.try_enter(retry), "second half-open probe must be rejected");
        expect(retry == std::chrono::milliseconds{std::stoi(policy(fixture, "probe_retry_ms"))},
               "probe retry delay diverged from shared fixture");
    }

    if (parse_bool(policy(fixture, "success_resets"))) {
        auto now = SpawnAdmissionGate::TimePoint{};
        auto gate = make_gate(fixture, now);
        const auto cooldown = gate.record_resource_failure();
        now += cooldown;
        expect(gate.try_enter(retry), "recovery probe must be admitted before success reset");
        gate.record_success();
        expect(gate.try_enter(retry), "success must reset the admission gate");
        expect(gate.try_enter(retry), "reset gate must allow normal concurrent starts");
    }

    if (parse_bool(policy(fixture, "non_resource_failure_preserves_streak"))) {
        auto now = SpawnAdmissionGate::TimePoint{};
        auto gate = make_gate(fixture, now);
        const auto cooldown = gate.record_resource_failure();
        now += cooldown;
        expect(gate.try_enter(retry), "recovery probe must be admitted before non-resource failure");
        gate.record_non_resource_failure();
        expect(gate.try_enter(retry), "non-resource failure must release the probe");
        expect(!gate.try_enter(retry), "non-resource failure must preserve the resource-pressure streak");
    }
}

} // namespace

int main()
{
    try {
        const auto fixture = load_fixture();
        test_classification_fixture(fixture);
        test_gate_fixture(fixture);
        std::cout << "mssv native shared conformance fixture: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv native shared conformance fixture: failed: " << exception.what() << '\n';
        return 1;
    }
}
