#include "mssv/linux_process_backend.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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

StderrSnapshot wait_for_complete_stderr(
    const LinuxProcessBackend& backend,
    const ChildId child_id,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto snapshot = backend.stderr_snapshot(child_id);
        expect(snapshot.has_value(), "FFmpeg stderr capture must exist while child is tracked");
        if (snapshot->complete) {
            return *snapshot;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("FFmpeg stderr capture did not reach EOF before timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

void test_synthetic_ffmpeg_natural_completion(const std::string& ffmpeg)
{
    constexpr std::size_t stderr_limit = 4096;
    LinuxProcessBackend backend(ProcessSpec{
        ffmpeg,
        {
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "warning",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=1000:sample_rate=48000:duration=0.35",
            "-f",
            "null",
            "-",
        },
        StderrCaptureOptions{stderr_limit, {}},
    });

    const auto attempt = backend.spawn();
    expect(attempt.started, "synthetic FFmpeg process must start");
    expect(
        backend.wait_for_exit(attempt.child_id, std::chrono::seconds{5}),
        "synthetic FFmpeg process must exit within the bounded wait");

    const auto stderr_snapshot = wait_for_complete_stderr(
        backend,
        attempt.child_id,
        std::chrono::seconds{2});
    expect(stderr_snapshot.text.size() <= stderr_limit, "FFmpeg stderr must stay within its capture bound");
    expect(backend.reap(attempt.child_id), "completed FFmpeg process ownership must be finalized");
    expect(backend.tracked_child_count() == 0, "completed FFmpeg process must not remain tracked");
}

void test_synthetic_ffmpeg_cancellation_cleanup(const std::string& ffmpeg)
{
    LinuxProcessBackend backend(ProcessSpec{
        ffmpeg,
        {
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "warning",
            "-re",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:sample_rate=48000",
            "-f",
            "null",
            "-",
        },
        StderrCaptureOptions{4096, {}},
    });
    SpawnAdmissionGate gate;
    ProcessSupervisor supervisor(backend, gate, true);

    const auto start = supervisor.start();
    expect(start.status == StartStatus::started, "long-running synthetic FFmpeg process must start");

    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    const auto stop = supervisor.stop(
        start.child_id,
        StopCause::cancellation,
        std::chrono::milliseconds{750},
        std::chrono::seconds{2});

    expect(stop.termination_requested, "FFmpeg cancellation must request process-group termination");
    expect(stop.cleanup_complete(), "FFmpeg cancellation must leave no tracked process group");
    expect(backend.tracked_child_count() == 0, "cancelled FFmpeg process must not remain tracked");
}

} // namespace

int main(const int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: ffmpeg_process_integration_tests <ffmpeg-path>\n";
        return 2;
    }

    try {
        const std::string ffmpeg(argv[1]);
        test_synthetic_ffmpeg_natural_completion(ffmpeg);
        test_synthetic_ffmpeg_cancellation_cleanup(ffmpeg);
        std::cout << "mssv FFmpeg process integration tests: ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "mssv FFmpeg process integration tests: failed: " << exception.what() << '\n';
        return 1;
    }
}
