#include "mssv/linux_process_backend.h"

#if !defined(__linux__)
#error "LinuxProcessBackend is only supported on Linux"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <system_error>
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
constexpr auto stderr_poll_interval = std::chrono::milliseconds{20};
constexpr std::string_view redaction_marker = "[redacted]";

bool contains_embedded_null(const std::string& value) noexcept
{
    return value.find('\0') != std::string::npos;
}

bool process_group_absent(const pid_t process_group) noexcept
{
    errno = 0;
    if (kill(-process_group, 0) == 0) {
        return false;
    }
    return errno == ESRCH;
}

bool promote_descriptor_above_standard_streams(int& descriptor) noexcept
{
    if (descriptor > STDERR_FILENO) {
        return true;
    }

    const auto promoted = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (promoted < 0) {
        return false;
    }

    (void)close(descriptor);
    descriptor = promoted;
    return true;
}

void remove_partial_redaction_suffix(
    std::string& text,
    const std::vector<std::string>& literals)
{
    for (const auto& literal : literals) {
        if (literal.empty() || text.empty()) {
            continue;
        }

        const auto maximum = std::min(text.size(), literal.size() - 1);
        for (std::size_t length = maximum; length > 0; --length) {
            if (text.compare(text.size() - length, length, literal, 0, length) == 0) {
                text.erase(text.size() - length);
                break;
            }
        }
    }
}

std::string redact_text(
    std::string text,
    const std::vector<std::string>& literals,
    const bool truncated,
    const std::size_t max_bytes)
{
    if (truncated) {
        remove_partial_redaction_suffix(text, literals);
    }

    for (const auto& literal : literals) {
        if (literal.empty()) {
            continue;
        }

        std::size_t position{0};
        while ((position = text.find(literal, position)) != std::string::npos) {
            text.replace(position, literal.size(), redaction_marker);
            position += redaction_marker.size();
        }
    }

    if (text.size() > max_bytes) {
        text.resize(max_bytes);
    }
    return text;
}

class StderrCapture final {
public:
    static std::unique_ptr<StderrCapture> create(
        const StderrCaptureOptions& options,
        int& native_error)
    {
        native_error = 0;
        if (options.max_bytes == 0 || options.max_bytes > max_stderr_capture_bytes) {
            native_error = EINVAL;
            return nullptr;
        }
        if (options.redact_literals.size() > max_stderr_redaction_literals) {
            native_error = EINVAL;
            return nullptr;
        }

        for (const auto& literal : options.redact_literals) {
            if (literal.size() > max_stderr_redaction_literal_bytes) {
                native_error = EINVAL;
                return nullptr;
            }
        }

        int descriptors[2]{-1, -1};
        if (pipe2(descriptors, O_CLOEXEC) != 0) {
            native_error = errno;
            return nullptr;
        }

        if (!promote_descriptor_above_standard_streams(descriptors[0])
            || !promote_descriptor_above_standard_streams(descriptors[1])) {
            native_error = errno != 0 ? errno : EMFILE;
            (void)close(descriptors[0]);
            (void)close(descriptors[1]);
            return nullptr;
        }

        std::unique_ptr<StderrCapture> capture;
        try {
            capture = std::unique_ptr<StderrCapture>(new StderrCapture(
                options.max_bytes,
                options.redact_literals,
                descriptors[0],
                descriptors[1]));
            capture->buffer_.reserve(options.max_bytes);
            capture->reader_ = std::thread([capture_ptr = capture.get()] {
                capture_ptr->drain();
            });
        } catch (const std::bad_alloc&) {
            if (capture != nullptr) {
                capture.reset();
            } else {
                (void)close(descriptors[0]);
                (void)close(descriptors[1]);
            }
            native_error = ENOMEM;
            return nullptr;
        } catch (const std::system_error& error) {
            if (capture != nullptr) {
                capture.reset();
            } else {
                (void)close(descriptors[0]);
                (void)close(descriptors[1]);
            }
            native_error = error.code().value() != 0 ? error.code().value() : EAGAIN;
            return nullptr;
        }

        return capture;
    }

    ~StderrCapture()
    {
        close_parent_write();
        stop_.store(true, std::memory_order_relaxed);
        if (reader_.joinable()) {
            reader_.join();
        }
        if (read_fd_ >= 0) {
            (void)close(read_fd_);
        }
    }

    [[nodiscard]] int read_fd() const noexcept
    {
        return read_fd_;
    }

    [[nodiscard]] int write_fd() const noexcept
    {
        return write_fd_;
    }

    void close_parent_write() noexcept
    {
        if (write_fd_ >= 0) {
            (void)close(write_fd_);
            write_fd_ = -1;
        }
    }

    [[nodiscard]] StderrSnapshot snapshot() const
    {
        std::string raw;
        bool truncated{false};
        {
            std::lock_guard lock(mutex_);
            raw = buffer_;
            truncated = truncated_;
        }

        auto redacted = redact_text(
            std::move(raw),
            redact_literals_,
            truncated,
            max_bytes_);

        return {
            std::move(redacted),
            truncated,
            complete_.load(std::memory_order_relaxed),
        };
    }

private:
    StderrCapture(
        const std::size_t max_bytes,
        std::vector<std::string> redact_literals,
        const int read_fd,
        const int write_fd)
        : max_bytes_(max_bytes),
          redact_literals_(std::move(redact_literals)),
          read_fd_(read_fd),
          write_fd_(write_fd)
    {
    }

    void drain() noexcept
    {
        std::array<char, 4096> chunk{};
        while (!stop_.load(std::memory_order_relaxed)) {
            pollfd descriptor{};
            descriptor.fd = read_fd_;
            descriptor.events = POLLIN | POLLHUP;

            const auto poll_result = poll(
                &descriptor,
                1,
                static_cast<int>(stderr_poll_interval.count()));
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (poll_result == 0) {
                continue;
            }

            if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
                const auto bytes_read = read(read_fd_, chunk.data(), chunk.size());
                if (bytes_read > 0) {
                    append(chunk.data(), static_cast<std::size_t>(bytes_read));
                    continue;
                }
                if (bytes_read == 0) {
                    complete_.store(true, std::memory_order_relaxed);
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                break;
            }
        }
    }

    void append(const char* data, const std::size_t size) noexcept
    {
        std::lock_guard lock(mutex_);
        if (buffer_.size() >= max_bytes_) {
            truncated_ = true;
            return;
        }

        const auto remaining = max_bytes_ - buffer_.size();
        const auto accepted = std::min(remaining, size);
        try {
            buffer_.append(data, accepted);
        } catch (...) {
            truncated_ = true;
            return;
        }
        if (accepted < size) {
            truncated_ = true;
        }
    }

    std::size_t max_bytes_;
    std::vector<std::string> redact_literals_;
    int read_fd_{-1};
    int write_fd_{-1};
    mutable std::mutex mutex_;
    std::string buffer_;
    bool truncated_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> complete_{false};
    std::thread reader_;
};

} // namespace

struct LinuxProcessBackend::TrackedChild {
    ChildId child_id{0};
    bool leader_reaped{false};
    std::unique_ptr<StderrCapture> stderr_capture;
};

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

    std::unique_ptr<StderrCapture> stderr_capture;
    if (spec_.stderr_capture.has_value()) {
        int capture_error{0};
        stderr_capture = StderrCapture::create(*spec_.stderr_capture, capture_error);
        if (stderr_capture == nullptr) {
            return {false, 0, capture_error != 0 ? capture_error : EIO};
        }
    }

    std::unique_ptr<TrackedChild> tracked_child;
    try {
        tracked_child = std::make_unique<TrackedChild>();
    } catch (const std::bad_alloc&) {
        return {false, 0, ENOMEM};
    }
    tracked_child->stderr_capture = std::move(stderr_capture);

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

    posix_spawn_file_actions_t file_actions{};
    bool file_actions_initialized{false};
    if (tracked_child->stderr_capture != nullptr) {
        native_error = posix_spawn_file_actions_init(&file_actions);
        file_actions_initialized = native_error == 0;
        if (native_error == 0) {
            native_error = posix_spawn_file_actions_adddup2(
                &file_actions,
                tracked_child->stderr_capture->write_fd(),
                STDERR_FILENO);
        }
        if (native_error == 0) {
            native_error = posix_spawn_file_actions_addclose(
                &file_actions,
                tracked_child->stderr_capture->read_fd());
        }
        if (native_error == 0) {
            native_error = posix_spawn_file_actions_addclose(
                &file_actions,
                tracked_child->stderr_capture->write_fd());
        }
    }

    if (native_error == 0) {
        native_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }
    if (native_error == 0) {
        native_error = posix_spawnattr_setpgroup(&attributes, 0);
    }

    pid_t process_id{-1};
    if (native_error == 0) {
        native_error = posix_spawn(
            &process_id,
            spec_.executable.c_str(),
            file_actions_initialized ? &file_actions : nullptr,
            &attributes,
            argv.data(),
            environ);
    }

    if (file_actions_initialized) {
        (void)posix_spawn_file_actions_destroy(&file_actions);
    }
    (void)posix_spawnattr_destroy(&attributes);

    if (tracked_child->stderr_capture != nullptr) {
        tracked_child->stderr_capture->close_parent_write();
    }

    if (native_error != 0 || process_id <= 0) {
        return {false, 0, native_error != 0 ? native_error : ECHILD};
    }

    const auto child_id = static_cast<ChildId>(process_id);
    tracked_child->child_id = child_id;
    tracked_children_.push_back(std::move(tracked_child));
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
    auto* tracked = find_tracked(child_id);
    if (tracked == nullptr
        || child_id == 0
        || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    const auto process_id = static_cast<pid_t>(child_id);
    const auto bounded_timeout = timeout < std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds::zero()
        : timeout;
    const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;

    while (true) {
        if (!tracked->leader_reaped) {
            int status{0};
            const auto wait_result = waitpid(process_id, &status, WNOHANG);
            if (wait_result == process_id) {
                tracked->leader_reaped = true;
            } else if (wait_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
        }

        if (tracked->leader_reaped && process_group_absent(process_id)) {
            return true;
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
    const auto tracked = std::find_if(
        tracked_children_.begin(),
        tracked_children_.end(),
        [child_id](const auto& child) { return child->child_id == child_id; });
    if (tracked == tracked_children_.end()
        || child_id == 0
        || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    const auto process_id = static_cast<pid_t>(child_id);
    if (!(*tracked)->leader_reaped) {
        int status{0};
        while (true) {
            const auto wait_result = waitpid(process_id, &status, WNOHANG);
            if (wait_result == process_id) {
                (*tracked)->leader_reaped = true;
                break;
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

    if (!process_group_absent(process_id)) {
        return false;
    }

    tracked_children_.erase(tracked);
    return true;
}

std::size_t LinuxProcessBackend::tracked_child_count() const noexcept
{
    return tracked_children_.size();
}

std::optional<StderrSnapshot> LinuxProcessBackend::stderr_snapshot(const ChildId child_id) const
{
    const auto* tracked = find_tracked(child_id);
    if (tracked == nullptr || tracked->stderr_capture == nullptr) {
        return std::nullopt;
    }
    return tracked->stderr_capture->snapshot();
}

LinuxProcessBackend::TrackedChild* LinuxProcessBackend::find_tracked(const ChildId child_id) noexcept
{
    const auto tracked = std::find_if(
        tracked_children_.begin(),
        tracked_children_.end(),
        [child_id](const auto& child) { return child->child_id == child_id; });
    return tracked == tracked_children_.end() ? nullptr : tracked->get();
}

const LinuxProcessBackend::TrackedChild* LinuxProcessBackend::find_tracked(
    const ChildId child_id) const noexcept
{
    const auto tracked = std::find_if(
        tracked_children_.begin(),
        tracked_children_.end(),
        [child_id](const auto& child) { return child->child_id == child_id; });
    return tracked == tracked_children_.end() ? nullptr : tracked->get();
}

bool LinuxProcessBackend::is_tracked(const ChildId child_id) const noexcept
{
    return find_tracked(child_id) != nullptr;
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
    for (const auto& child : tracked_children_) {
        const auto child_id = child->child_id;
        if (child_id == 0 || child_id > static_cast<ChildId>(std::numeric_limits<pid_t>::max())) {
            continue;
        }
        const auto process_id = static_cast<pid_t>(child_id);
        (void)kill(-process_id, SIGKILL);
    }

    for (const auto& child : tracked_children_) {
        if (child->leader_reaped) {
            continue;
        }

        const auto child_id = child->child_id;
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
