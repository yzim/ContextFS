#include "backends/ptrace_backend.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef AGENTVFS_PTRACE
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(AGENTVFS_PTRACE) && defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define AGENTVFS_TEST_HAS_OPENAT2_H 1
#endif
#endif

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expr);                                   \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

static cas::OpMask op_bit(cas::OpType op) {
    return 1u << static_cast<unsigned>(op);
}

#ifdef AGENTVFS_PTRACE
namespace {

struct PipePair {
    int read_fd = -1;
    int write_fd = -1;
};

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

PipePair make_pipe() {
    int fds[2] = {-1, -1};
    CHECK(pipe(fds) == 0);
    return PipePair{fds[0], fds[1]};
}

bool write_byte(int fd) {
    char byte = 'x';
    return write(fd, &byte, 1) == 1;
}

bool read_byte_with_timeout(int fd, int timeout_ms) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP;

    while (true) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            return false;
        }
        char byte = 0;
        return read(fd, &byte, 1) == 1;
    }
}

std::string make_temp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-ptrace-") + suffix +
                        "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
    CHECK(path != nullptr);
    return std::string(path);
}

std::string extra_value(const cas::TelemetryEvent& ev,
                        const std::string& key) {
    for (const auto& entry : ev.extra) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return {};
}

long extra_long(const cas::TelemetryEvent& ev, const std::string& key) {
    std::string value = extra_value(ev, key);
    if (value.empty()) {
        return 0;
    }
    return std::strtol(value.c_str(), nullptr, 10);
}

bool wait_for_event(
    const std::vector<cas::TelemetryEvent>& events,
    std::mutex& events_mutex,
    const std::function<bool(const cas::TelemetryEvent&)>& predicate,
    int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            for (const auto& ev : events) {
                if (predicate(ev)) {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<cas::TelemetryEvent> event_snapshot(
    const std::vector<cas::TelemetryEvent>& events,
    std::mutex& events_mutex) {
    std::lock_guard<std::mutex> lock(events_mutex);
    return events;
}

void cleanup_child(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
}

void run_child_file_syscalls(const std::string& dir, int start_fd,
                             int done_fd, int stop_fd) {
    char byte = 0;
    if (syscall(SYS_read, start_fd, &byte, 1) != 1) {
        _exit(2);
    }

    std::string path = dir + "/io.txt";
    const char payload[] = "abc";
    int fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path.c_str(),
                                      O_CREAT | O_WRONLY | O_TRUNC, 0600));
    if (fd >= 0) {
        syscall(SYS_write, fd, payload, 3);
        syscall(SYS_close, fd);
    }

    fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path.c_str(),
                                  O_RDONLY));
    if (fd >= 0) {
        char buf[8] {};
        syscall(SYS_read, fd, buf, sizeof(buf));
        syscall(SYS_close, fd);
    }

#if defined(SYS_openat2) && defined(AGENTVFS_TEST_HAS_OPENAT2_H)
    std::string openat2_path = dir + "/openat2.txt";
    struct open_how how {};
    how.flags = O_CREAT | O_WRONLY | O_TRUNC;
    how.mode = 0600;
    fd = static_cast<int>(syscall(SYS_openat2, AT_FDCWD,
                                  openat2_path.c_str(), &how, sizeof(how)));
    if (fd >= 0) {
        syscall(SYS_close, fd);
    }
#endif

    syscall(SYS_write, done_fd, "d", 1);
    syscall(SYS_read, stop_fd, &byte, 1);
    _exit(0);
}

bool wait_until_process_gone(pid_t pid, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        errno = 0;
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace
#endif

static void test_name_and_capabilities() {
    cas::PtraceBackend backend;
    cas::Capabilities caps = backend.capabilities();

    CHECK(backend.name() == "ptrace");
#if defined(__x86_64__)
    cas::OpMask all_ops = op_bit(cas::OpType::Read) |
                          op_bit(cas::OpType::Write) |
                          op_bit(cas::OpType::Open) |
                          op_bit(cas::OpType::Close) |
                          op_bit(cas::OpType::Unlink) |
                          op_bit(cas::OpType::Rename) |
                          op_bit(cas::OpType::Truncate) |
                          op_bit(cas::OpType::Stat) |
                          op_bit(cas::OpType::Exec) |
                          op_bit(cas::OpType::Create);
    CHECK(caps.supported_ops == all_ops);
#else
    // On non-x86_64 the register-extraction path in get_syscall_state is a
    // no-op and attach_pid returns false, so we must not advertise any
    // supported ops. See PtraceBackend::capabilities() for the rationale.
    CHECK(caps.supported_ops == 0);
#endif
    CHECK(!caps.pre_op_verdicts);
    CHECK(!caps.requires_cgroup);
    CHECK(!caps.requires_root);
}

static void test_start_without_pids_succeeds_and_stop_is_safe() {
    cas::PtraceBackend backend;
    cas::BackendConfig cfg{};
    bool callback_called = false;

    CHECK(backend.start(cfg, [&](cas::TelemetryEvent) {
        callback_called = true;
    }));
    CHECK(!callback_called);
    backend.stop();
    backend.stop();
}

static void test_session_and_policy_hooks_accept_current_contract() {
    cas::PtraceBackend backend;

    CHECK(backend.register_session(cas::SessionInfo{}));
    CHECK(backend.unregister_session("/sys/fs/cgroup/session"));
    CHECK(backend.install_policy(cas::PolicyRules{}));
}

#ifdef AGENTVFS_PTRACE
static void test_enabled_ptrace_emits_events_and_stops_cleanly() {
#if !defined(__x86_64__)
    std::fprintf(stderr,
                 "SKIP test_enabled_ptrace_emits_events_and_stops_cleanly: "
                 "unsupported ptrace register ABI\n");
    return;
#else
    std::string dir = make_temp_dir("events");
    PipePair start_pipe = make_pipe();
    PipePair done_pipe = make_pipe();
    PipePair stop_pipe = make_pipe();

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close_fd(start_pipe.write_fd);
        close_fd(done_pipe.read_fd);
        close_fd(stop_pipe.write_fd);
        run_child_file_syscalls(dir, start_pipe.read_fd, done_pipe.write_fd,
                                stop_pipe.read_fd);
    }

    close_fd(start_pipe.read_fd);
    close_fd(done_pipe.write_fd);
    close_fd(stop_pipe.read_fd);

    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;
    std::atomic<bool> after_stop{false};
    std::atomic<int> callbacks_after_stop{0};

    cas::PtraceBackend backend;
    cas::BackendConfig cfg{};
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        if (after_stop.load()) {
            ++callbacks_after_stop;
        }
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    errno = 0;
    if (!backend.attach_pid(child)) {
        int attach_errno = errno;
        write_byte(stop_pipe.write_fd);
        cleanup_child(child);
        backend.stop();
        close_fd(start_pipe.write_fd);
        close_fd(done_pipe.read_fd);
        close_fd(stop_pipe.write_fd);
        rmdir(dir.c_str());
        std::fprintf(stderr,
                     "SKIP test_enabled_ptrace_emits_events_and_stops_cleanly: "
                     "ptrace attach failed errno=%d (%s)\n",
                     attach_errno, std::strerror(attach_errno));
        return;
    }

    CHECK(write_byte(start_pipe.write_fd));
    CHECK(read_byte_with_timeout(done_pipe.read_fd, 5000));

    CHECK(wait_for_event(events, events_mutex,
                         [](const cas::TelemetryEvent& ev) {
                             return ev.backend == "ptrace" &&
                                    ev.op == cas::OpType::Write &&
                                    extra_long(ev, "ret") == 3 &&
                                    ev.bytes == 3;
                         },
                         5000));
    CHECK(wait_for_event(events, events_mutex,
                         [](const cas::TelemetryEvent& ev) {
                             return ev.backend == "ptrace" &&
                                    ev.op == cas::OpType::Read &&
                                    extra_long(ev, "ret") > 0 &&
                                    ev.bytes == static_cast<uint64_t>(
                                                    extra_long(ev, "ret"));
                         },
                         5000));

#if defined(SYS_openat2) && defined(AGENTVFS_TEST_HAS_OPENAT2_H)
    CHECK(wait_for_event(events, events_mutex,
                         [](const cas::TelemetryEvent& ev) {
                             return extra_long(ev, "syscall_nr") ==
                                    SYS_openat2;
                         },
                         5000));
    for (const auto& ev : event_snapshot(events, events_mutex)) {
        if (extra_long(ev, "syscall_nr") == SYS_openat2 &&
            extra_long(ev, "ret") != -ENOSYS) {
            CHECK(ev.op == cas::OpType::Create);
        }
    }
#endif

    backend.stop();
    after_stop.store(true);
    int callbacks_at_stop = callbacks_after_stop.load();
    CHECK(write_byte(stop_pipe.write_fd));
    cleanup_child(child);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(callbacks_after_stop.load() == callbacks_at_stop);

    close_fd(start_pipe.write_fd);
    close_fd(done_pipe.read_fd);
    close_fd(stop_pipe.write_fd);
    unlink((dir + "/io.txt").c_str());
    unlink((dir + "/openat2.txt").c_str());
    rmdir(dir.c_str());
#endif
}

static void test_exec_does_not_emit_garbage_paired_events() {
#if !defined(__x86_64__)
    std::fprintf(stderr,
                 "SKIP test_exec_does_not_emit_garbage_paired_events: "
                 "unsupported ptrace register ABI\n");
    return;
#else
    // Pick an exec target that exists on this system. /bin/true is the
    // canonical choice; fall back to /usr/bin/true.
    const char* exec_target = nullptr;
    if (access("/bin/true", X_OK) == 0) {
        exec_target = "/bin/true";
    } else if (access("/usr/bin/true", X_OK) == 0) {
        exec_target = "/usr/bin/true";
    } else {
        std::fprintf(stderr,
                     "SKIP test_exec_does_not_emit_garbage_paired_events: "
                     "no /bin/true or /usr/bin/true available\n");
        return;
    }

    PipePair start_pipe = make_pipe();

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close_fd(start_pipe.write_fd);
        // Wait for the tracer to attach before doing anything interesting.
        char byte = 0;
        if (syscall(SYS_read, start_pipe.read_fd, &byte, 1) != 1) {
            _exit(2);
        }
        // Drop pipe fd (close is itself an interesting syscall — exec
        // will then replace the process image).
        syscall(SYS_close, start_pipe.read_fd);
        char* const argv[] = {const_cast<char*>(exec_target), nullptr};
        char* const envp[] = {nullptr};
        execve(exec_target, argv, envp);
        _exit(3);  // exec failed
    }
    close_fd(start_pipe.read_fd);

    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::PtraceBackend backend;
    cas::BackendConfig cfg{};
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    errno = 0;
    if (!backend.attach_pid(child)) {
        int attach_errno = errno;
        write_byte(start_pipe.write_fd);
        cleanup_child(child);
        backend.stop();
        close_fd(start_pipe.write_fd);
        std::fprintf(stderr,
                     "SKIP test_exec_does_not_emit_garbage_paired_events: "
                     "ptrace attach failed errno=%d (%s)\n",
                     attach_errno, std::strerror(attach_errno));
        return;
    }

    CHECK(write_byte(start_pipe.write_fd));

    // Wait for the child to exit (it runs /bin/true after exec).
    CHECK(wait_until_process_gone(child, 5000));

    // Wait long enough for any pending exec event to surface.
    CHECK(wait_for_event(events, events_mutex,
                         [](const cas::TelemetryEvent& ev) {
                             // Either the synthesized exec event...
                             if (ev.op == cas::OpType::Exec) {
                                 std::string tag;
                                 for (const auto& e : ev.extra) {
                                     if (e.first == "event") {
                                         tag = e.second;
                                         break;
                                     }
                                 }
                                 if (tag == "exec") {
                                     return true;
                                 }
                             }
                             return false;
                         },
                         5000));

    backend.stop();

    // Pairing-correctness invariant: no event may carry a non-error return
    // value belonging to syscall_nr=SYS_execve, because execve never
    // returns on success — if we ever emit one, it means the entry
    // snapshot from before exec was wrongly paired with the
    // post-exec-image syscall-exit.
    auto snap = event_snapshot(events, events_mutex);
    for (const auto& ev : snap) {
        long nr = extra_long(ev, "syscall_nr");
        long ret = extra_long(ev, "ret");
        if (nr == SYS_execve) {
            // Successful execve never produces a regular paired event.
            // Only an error path (ret < 0) should ever appear here.
            CHECK(ret < 0);
        }
    }

    cleanup_child(child);
    close_fd(start_pipe.write_fd);
#endif
}

static void test_exited_tracee_is_reaped_before_duplicate_attach() {
#if !defined(__x86_64__)
    std::fprintf(stderr,
                 "SKIP test_exited_tracee_is_reaped_before_duplicate_attach: "
                 "unsupported ptrace register ABI\n");
    return;
#else
    PipePair start_pipe = make_pipe();

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close_fd(start_pipe.write_fd);
        char byte = 0;
        syscall(SYS_read, start_pipe.read_fd, &byte, 1);
        _exit(0);
    }
    close_fd(start_pipe.read_fd);

    cas::PtraceBackend backend;
    CHECK(backend.start(cas::BackendConfig{}, [](cas::TelemetryEvent) {}));

    errno = 0;
    if (!backend.attach_pid(child)) {
        int attach_errno = errno;
        write_byte(start_pipe.write_fd);
        cleanup_child(child);
        backend.stop();
        close_fd(start_pipe.write_fd);
        std::fprintf(stderr,
                     "SKIP test_exited_tracee_is_reaped_before_duplicate_attach: "
                     "ptrace attach failed errno=%d (%s)\n",
                     attach_errno, std::strerror(attach_errno));
        return;
    }

    CHECK(write_byte(start_pipe.write_fd));
    CHECK(wait_until_process_gone(child, 5000));
    CHECK(!backend.attach_pid(child));
    backend.stop();
    close_fd(start_pipe.write_fd);
#endif
}
#endif

int main() {
    test_name_and_capabilities();
    test_start_without_pids_succeeds_and_stop_is_safe();
    test_session_and_policy_hooks_accept_current_contract();
#ifdef AGENTVFS_PTRACE
    test_enabled_ptrace_emits_events_and_stops_cleanly();
    test_exec_does_not_emit_garbage_paired_events();
    test_exited_tracee_is_reaped_before_duplicate_attach();
#endif
    std::fprintf(stderr, "test_ptrace_backend: all tests passed\n");
    return 0;
}
