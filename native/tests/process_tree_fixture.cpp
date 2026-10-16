#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t terminate_requested = 0;

extern "C" void handle_terminate(int)
{
    terminate_requested = 1;
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

int run_tree_graceful(const std::string& child_pid_path)
{
    const auto child = fork();
    if (child < 0) {
        return 10;
    }

    if (child == 0) {
        while (true) {
            pause();
        }
    }

    struct sigaction action {};
    action.sa_handler = handle_terminate;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGTERM, &action, nullptr) != 0) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, nullptr, 0);
        return 11;
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

int run_ignore_term(const std::string& ready_path)
{
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGTERM, &action, nullptr) != 0) {
        return 20;
    }

    if (!write_pid_file(ready_path, getpid())) {
        return 21;
    }

    while (true) {
        pause();
    }
}

} // namespace

int main(const int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: process_tree_fixture <tree-graceful|ignore-term> <marker-path>\n";
        return 2;
    }

    const std::string mode(argv[1]);
    const std::string marker_path(argv[2]);

    if (mode == "tree-graceful") {
        return run_tree_graceful(marker_path);
    }
    if (mode == "ignore-term") {
        return run_ignore_term(marker_path);
    }

    std::cerr << "unknown mode\n";
    return 3;
}
