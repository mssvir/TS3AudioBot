#include "mssv/linux_process_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

using mssv::process::ChildId;
using mssv::process::LinuxProcessBackend;
using mssv::process::ProcessSpec;

struct ProcessSample {
    std::uint64_t cpu_ticks{0};
    std::uint64_t rss_kib{0};
    std::uint64_t threads{0};
};

struct ChildProcess {
    std::unique_ptr<LinuxProcessBackend> backend;
    ChildId child_id{0};
};

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::uint64_t parse_unsigned(const std::string& value, const std::string_view field_name)
{
    std::size_t consumed{0};
    const auto parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) {
        throw std::runtime_error("invalid numeric value for " + std::string(field_name));
    }
    return parsed;
}

std::uint64_t read_cpu_ticks(const ChildId child_id)
{
    std::ifstream input("/proc/" + std::to_string(child_id) + "/stat");
    expect(static_cast<bool>(input), "unable to read child /proc stat");

    std::string line;
    std::getline(input, line);
    const auto command_end = line.rfind(") ");
    expect(command_end != std::string::npos, "unexpected /proc stat format");

    std::istringstream fields(line.substr(command_end + 2));
    std::vector<std::string> tokens;
    for (std::string token; fields >> token;) {
        tokens.push_back(std::move(token));
    }

    // tokens[0] is field 3 (state), so fields 14/15 (utime/stime) are 11/12.
    expect(tokens.size() > 12, "incomplete /proc stat data");
    return parse_unsigned(tokens[11], "utime") + parse_unsigned(tokens[12], "stime");
}

ProcessSample read_process_sample(const ChildId child_id)
{
    ProcessSample sample{};
    sample.cpu_ticks = read_cpu_ticks(child_id);

    std::ifstream input("/proc/" + std::to_string(child_id) + "/status");
    expect(static_cast<bool>(input), "unable to read child /proc status");

    bool saw_rss{false};
    bool saw_threads{false};
    for (std::string line; std::getline(input, line);) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream value(line.substr(6));
            value >> sample.rss_kib;
            saw_rss = static_cast<bool>(value);
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream value(line.substr(8));
            value >> sample.threads;
            saw_threads = static_cast<bool>(value);
        }
    }

    expect(saw_rss, "child /proc status did not expose VmRSS");
    expect(saw_threads, "child /proc status did not expose Threads");
    return sample;
}

std::vector<ProcessSample> sample_all(const std::vector<ChildProcess>& children)
{
    std::vector<ProcessSample> samples;
    samples.reserve(children.size());
    for (const auto& child : children) {
        samples.push_back(read_process_sample(child.child_id));
    }
    return samples;
}

std::uint64_t sum_cpu_ticks(const std::vector<ProcessSample>& samples)
{
    std::uint64_t total{0};
    for (const auto& sample : samples) {
        total += sample.cpu_ticks;
    }
    return total;
}

std::uint64_t sum_rss_kib(const std::vector<ProcessSample>& samples)
{
    std::uint64_t total{0};
    for (const auto& sample : samples) {
        total += sample.rss_kib;
    }
    return total;
}

std::uint64_t sum_threads(const std::vector<ProcessSample>& samples)
{
    std::uint64_t total{0};
    for (const auto& sample : samples) {
        total += sample.threads;
    }
    return total;
}

std::size_t parse_fanout(const char* value)
{
    const std::string text(value);
    const auto parsed = parse_unsigned(text, "fanout");
    expect(parsed >= 1 && parsed <= 16, "fanout must be between 1 and 16");
    return static_cast<std::size_t>(parsed);
}

} // namespace

int main(const int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: ffmpeg_resource_baseline <ffmpeg-path> [fanout]\n";
        return 2;
    }

    try {
        const std::string ffmpeg(argv[1]);
        const auto fanout = argc == 3 ? parse_fanout(argv[2]) : std::size_t{6};

        std::vector<ChildProcess> children;
        children.reserve(fanout);

        for (std::size_t index = 0; index < fanout; ++index) {
            const auto frequency = 440U + static_cast<unsigned>(index * 17U);
            auto backend = std::make_unique<LinuxProcessBackend>(ProcessSpec{
                ffmpeg,
                {
                    "-hide_banner",
                    "-nostdin",
                    "-loglevel",
                    "error",
                    "-re",
                    "-f",
                    "lavfi",
                    "-i",
                    "sine=frequency=" + std::to_string(frequency) + ":sample_rate=48000",
                    "-t",
                    "4",
                    "-f",
                    "null",
                    "-",
                },
                std::nullopt,
            });

            const auto attempt = backend->spawn();
            expect(attempt.started, "representative FFmpeg fan-out child failed to start");
            children.push_back({std::move(backend), attempt.child_id});
        }

        const auto initial_samples = sample_all(children);
        const auto initial_cpu_ticks = sum_cpu_ticks(initial_samples);
        auto peak_rss_kib = sum_rss_kib(initial_samples);
        auto peak_threads = sum_threads(initial_samples);

        constexpr auto measurement_window = std::chrono::milliseconds{1500};
        constexpr auto sample_interval = std::chrono::milliseconds{100};
        const auto measurement_start = std::chrono::steady_clock::now();
        auto final_samples = initial_samples;

        while (std::chrono::steady_clock::now() - measurement_start < measurement_window) {
            std::this_thread::sleep_for(sample_interval);
            final_samples = sample_all(children);
            peak_rss_kib = std::max(peak_rss_kib, sum_rss_kib(final_samples));
            peak_threads = std::max(peak_threads, sum_threads(final_samples));
        }

        const auto measurement_end = std::chrono::steady_clock::now();
        const auto final_cpu_ticks = sum_cpu_ticks(final_samples);
        expect(final_cpu_ticks >= initial_cpu_ticks, "aggregate FFmpeg CPU ticks must be monotonic");
        expect(peak_rss_kib > 0, "representative FFmpeg fan-out must expose nonzero RSS");
        expect(peak_threads >= fanout, "representative FFmpeg fan-out must expose at least one thread per child");

        const auto clock_ticks_per_second = sysconf(_SC_CLK_TCK);
        expect(clock_ticks_per_second > 0, "unable to resolve Linux clock ticks per second");
        const auto elapsed_seconds = std::chrono::duration<double>(measurement_end - measurement_start).count();
        expect(elapsed_seconds > 0.0, "resource measurement window must be positive");

        const auto cpu_seconds = static_cast<double>(final_cpu_ticks - initial_cpu_ticks)
            / static_cast<double>(clock_ticks_per_second);
        const auto aggregate_cpu_percent = (cpu_seconds / elapsed_seconds) * 100.0;

        std::size_t finalized{0};
        for (auto& child : children) {
            expect(
                child.backend->wait_for_exit(child.child_id, std::chrono::seconds{8}),
                "representative FFmpeg fan-out child did not exit within bounded wait");
            expect(child.backend->reap(child.child_id), "representative FFmpeg fan-out ownership did not finalize");
            expect(child.backend->tracked_child_count() == 0, "finalized FFmpeg child remained tracked");
            ++finalized;
        }

        std::cout << "mssv_ffmpeg_resource_baseline_v1\n";
        std::cout << "fanout=" << fanout << '\n';
        std::cout << "measurement_window_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(measurement_end - measurement_start).count()
                  << '\n';
        std::cout << std::fixed << std::setprecision(2)
                  << "aggregate_cpu_percent=" << aggregate_cpu_percent << '\n';
        std::cout << "peak_aggregate_rss_kib=" << peak_rss_kib << '\n';
        std::cout << "peak_aggregate_threads=" << peak_threads << '\n';
        std::cout << "started=" << children.size() << '\n';
        std::cout << "finalized=" << finalized << '\n';
        std::cout << "cleanup_complete=" << (finalized == children.size() ? "true" : "false") << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv FFmpeg resource baseline: failed: " << exception.what() << '\n';
        return 1;
    }
}
