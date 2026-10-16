#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t terminate_requested = 0;

extern "C" void handle_terminate(int)
{
    terminate_requested = 1;
}

bool write_all(const int descriptor, const std::string_view text)
{
    std::size_t offset{0};
    while (offset < text.size()) {
        const auto written = write(descriptor, text.data() + offset, text.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool write_pid_file(const std::string& path, const pid_t process_id)
{
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << process_id << '\n';
    output.flush();
    return static_cast<bool>(output);
}

bool install_terminate_handler()
{
    struct sigaction action {};
    action.sa_handler = handle_terminate;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGTERM, &action, nullptr) == 0;
}

bool use_default_terminate()
{
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGTERM, &action, nullptr) == 0;
}

bool ignore_terminate()
{
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGTERM, &action, nullptr) == 0;
}

int run_tree_graceful(const std::string& child_pid_path)
{
    if (!install_terminate_handler()) {
        return 10;
    }

    const auto child = fork();
    if (child < 0) {
        return 11;
    }

    if (child == 0) {
        if (!use_default_terminate()) {
            _exit(14);
        }
        while (true) {
            pause();
        }
    }

    if (!write_pid_file(child_pid_path, child)) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, nullptr, 0);
        return 12;
    }

    while (terminate_requested == 0) {
        pause();
    }

    (void)kill(child, SIGTERM);
    int status{0};
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 13;
        }
    }

    return 0;
}

int run_tree_stubborn(const std::string& child_pid_path)
{
    if (!install_terminate_handler()) {
        return 40;
    }

    const auto child = fork();
    if (child < 0) {
        return 41;
    }

    if (child == 0) {
        if (!ignore_terminate()) {
            _exit(42);
        }
        if (!write_pid_file(child_pid_path, getpid())) {
            _exit(43);
        }
        while (true) {
            pause();
        }
    }

    while (terminate_requested == 0) {
        pause();
    }

    // Deliberately exit without reaping or killing the SIGTERM-ignoring descendant.
    // The supervisor must not declare the owned process group clean until it is gone.
    return 0;
}

int run_ignore_term(const std::string& ready_path)
{
    if (!ignore_terminate()) {
        return 20;
    }

    if (!write_pid_file(ready_path, getpid())) {
        return 21;
    }

    while (true) {
        pause();
    }
}

int run_emit_stderr(const std::string& secret)
{
    if (!write_all(STDERR_FILENO, "diagnostic-before-")) {
        return 30;
    }

    const auto split = secret.size() / 2;
    if (!write_all(STDERR_FILENO, std::string_view(secret).substr(0, split))) {
        return 31;
    }
    if (!write_all(STDERR_FILENO, std::string_view(secret).substr(split))) {
        return 32;
    }

    const std::string chunk(4096, 'X');
    for (int index = 0; index < 256; ++index) {
        if (!write_all(STDERR_FILENO, chunk)) {
            return 33;
        }
    }

    return 0;
}

} // namespace

int main(const int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: process_tree_fixture <tree-graceful|tree-stubborn|ignore-term|emit-stderr> <value>\n";
        return 2;
    }

    const std::string mode(argv[1]);
    const std::string value(argv[2]);

    if (mode == "tree-graceful") {
        return run_tree_graceful(value);
    }
    if (mode == "tree-stubborn") {
        return run_tree_stubborn(value);
    }
    if (mode == "ignore-term") {
        return run_ignore_term(value);
    }
    if (mode == "emit-stderr") {
        return run_emit_stderr(value);
    }

    std::cerr << "unknown mode\n";
    return 3;
}
