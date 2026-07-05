#include "runtime_process_posix.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

namespace {

bool wait_for_exit(pid_t pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == ECHILD) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void cleanup_child(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void test_terminate_stopped_process_group_returns_quickly() {
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        setpgid(0, 0);
        while (true) pause();
    }

    bool child_owned = true;
    auto cleanup = [&]() {
        if (child_owned) cleanup_child(child);
        child_owned = false;
    };

    REQUIRE(setpgid(child, child) == 0 || errno == EACCES);
    REQUIRE(kill(-child, SIGSTOP) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    PosixRuntimeProcessController pc;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    bool ok = pc.terminate_process_group(child, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    REQUIRE(ok);
    REQUIRE(error.empty());
    REQUIRE(elapsed < std::chrono::milliseconds(150));
    REQUIRE(wait_for_exit(child, 250));
    child_owned = false;
    cleanup();
}

} // namespace

int main() {
    std::printf("test_runtime_process_posix:\n");
    test_terminate_stopped_process_group_returns_quickly();
    std::printf("PASS test_runtime_process_posix\n");
    return 0;
}
