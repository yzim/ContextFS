#include "backends/ldpreload_backend.h"
#include "backends/ldpreload_protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expr);                                   \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

namespace {

using PreloadMsg = cas::ldpreload_protocol::PreloadMsg;

static cas::OpMask op_bit(cas::OpType op) {
    return 1u << static_cast<unsigned>(op);
}

static std::string make_socket_path(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-ldpreload-") + suffix +
                        "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    CHECK(dir != nullptr);
    return std::string(dir) + "/cas_preload.sock";
}

static std::string socket_dir(const std::string& path) {
    return path.substr(0, path.find_last_of('/'));
}

static void remove_socket_tree(const std::string& path) {
    unlink(path.c_str());
    rmdir(socket_dir(path).c_str());
}

static int connect_client(const std::string& socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    CHECK(socket_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    CHECK(connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0);
    return fd;
}

static bool try_connect_client(const std::string& socket_path, int& out_fd) {
    out_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(out_fd >= 0);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    CHECK(socket_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(out_fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) == 0) {
        return true;
    }
    close(out_fd);
    out_fd = -1;
    return false;
}

static int bind_listening_socket(const std::string& socket_path,
                                 int backlog = 1) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    CHECK(socket_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(socket_path.c_str());
    CHECK(bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) == 0);
    CHECK(listen(fd, backlog) == 0);
    return fd;
}

static int start_nonblocking_connect(const std::string& socket_path,
                                     bool& backlog_full) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    int flags = fcntl(fd, F_GETFL);
    CHECK(flags >= 0);
    CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    CHECK(socket_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) == 0) {
        return fd;
    }

    if (errno == EINPROGRESS || errno == EALREADY || errno == EISCONN) {
        return fd;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        backlog_full = true;
    }
    close(fd);
    return -1;
}

static void write_full(int fd, const void* data, size_t len) {
    const auto* cursor = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = write(fd, cursor, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        CHECK(n > 0);
        cursor += n;
        len -= static_cast<size_t>(n);
    }
}

static bool read_verdict(int fd, char& verdict, int timeout_ms = 2000) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
        return false;
    }

    ssize_t n = read(fd, &verdict, 1);
    return n == 1;
}

static bool wait_for_event_count(const std::vector<cas::TelemetryEvent>& events,
                                 std::mutex& events_mutex,
                                 size_t expected,
                                 int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            if (events.size() >= expected) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

static void test_name_and_capabilities() {
    cas::LdpreloadBackend backend;
    cas::Capabilities caps = backend.capabilities();

    CHECK(backend.name() == "ldpreload");
    CHECK(caps.supported_ops & op_bit(cas::OpType::Read));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Write));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Open));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Close));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Truncate));
    CHECK(caps.supported_ops & op_bit(cas::OpType::Stat));
    CHECK(!(caps.supported_ops & op_bit(cas::OpType::Unlink)));
    CHECK(!(caps.supported_ops & op_bit(cas::OpType::Rename)));
    // The probe library blocks the syscall, sends an event, and translates a
    // Verdict::Deny verdict to errno=EACCES BEFORE invoking the real libc
    // call, so this backend is genuinely a pre-op verdict source.
    CHECK(caps.pre_op_verdicts);
    CHECK(!caps.requires_cgroup);
    CHECK(!caps.requires_root);
}

static void test_default_socket_path_uses_xdg_runtime_dir_when_set() {
    // Save and restore environment so this test is isolated.
    std::string saved_xdg;
    bool had_xdg = false;
    if (const char* prev = std::getenv("XDG_RUNTIME_DIR")) {
        saved_xdg = prev;
        had_xdg = true;
    }

    // Use a known-good directory we are sure exists so the stat() check
    // inside default_socket_path succeeds.
    std::string dir = "/tmp/agentvfs-xdg-test-XXXXXX";
    std::vector<char> buf(dir.begin(), dir.end());
    buf.push_back('\0');
    char* mk = mkdtemp(buf.data());
    CHECK(mk != nullptr);
    std::string xdg = mk;

    CHECK(setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1) == 0);
    std::string resolved = cas::ldpreload_default_socket_path();
    CHECK(resolved == xdg + "/cas_preload.sock");

    // Sanity: when XDG_RUNTIME_DIR is unset, it must fall back to a
    // uid-suffixed /tmp path, not the legacy shared /tmp/cas_preload.sock.
    CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
    std::string fallback = cas::ldpreload_default_socket_path();
    CHECK(fallback.find("/tmp/cas_preload-") == 0);
    CHECK(fallback.find(std::to_string(geteuid())) != std::string::npos);
    CHECK(fallback != "/tmp/cas_preload.sock");

    // Restore environment.
    if (had_xdg) {
        CHECK(setenv("XDG_RUNTIME_DIR", saved_xdg.c_str(), 1) == 0);
    }
    rmdir(xdg.c_str());
}

static void test_session_and_policy_hooks_accept_current_contract() {
    cas::LdpreloadBackend backend;

    CHECK(backend.register_session(cas::SessionInfo{}));
    CHECK(backend.unregister_session("/sys/fs/cgroup/session"));
    CHECK(backend.install_policy(cas::PolicyRules{}));
}

static void test_start_stop_cleans_socket_and_is_idempotent() {
    std::string socket_path = make_socket_path("cleanup");
    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;

    CHECK(backend.start(cfg, [](cas::TelemetryEvent) {}));
    struct stat st {};
    CHECK(stat(socket_path.c_str(), &st) == 0);
    backend.stop();
    backend.stop();
    CHECK(stat(socket_path.c_str(), &st) != 0 && errno == ENOENT);
    remove_socket_tree(socket_path);
}

static void test_start_hardens_socket_permissions() {
    std::string socket_path = make_socket_path("permissions");
    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;

    mode_t old_umask = umask(0);
    bool started = backend.start(cfg, [](cas::TelemetryEvent) {});
    umask(old_umask);
    CHECK(started);

    struct stat st {};
    CHECK(lstat(socket_path.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);

    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_start_rejects_too_long_socket_path() {
    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = std::string(200, 'x');

    CHECK(!backend.start(cfg, [](cas::TelemetryEvent) {}));
    backend.stop();
}

static void test_start_preserves_existing_non_socket_path() {
    std::string socket_path = make_socket_path("regular-file");
    int fd = open(socket_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(fd >= 0);
    const char contents[] = "not a socket";
    write_full(fd, contents, sizeof(contents) - 1);
    close(fd);

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;

    CHECK(!backend.start(cfg, [](cas::TelemetryEvent) {}));
    struct stat st {};
    CHECK(lstat(socket_path.c_str(), &st) == 0);
    CHECK(S_ISREG(st.st_mode));

    unlink(socket_path.c_str());
    remove_socket_tree(socket_path);
}

static void test_start_preserves_live_existing_socket() {
    std::string socket_path = make_socket_path("live-socket");
    int live_fd = bind_listening_socket(socket_path);

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;

    CHECK(!backend.start(cfg, [](cas::TelemetryEvent) {}));
    struct stat st {};
    CHECK(lstat(socket_path.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));

    int client_fd = -1;
    CHECK(try_connect_client(socket_path, client_fd));
    close(client_fd);
    close(live_fd);
    unlink(socket_path.c_str());
    remove_socket_tree(socket_path);
}

static void test_start_preserves_live_socket_with_full_backlog() {
    std::string socket_path = make_socket_path("full-backlog");
    int live_fd = bind_listening_socket(socket_path, 0);
    std::vector<int> held_fds;
    bool backlog_full = false;

    for (int i = 0; i < 64 && !backlog_full; ++i) {
        int fd = start_nonblocking_connect(socket_path, backlog_full);
        if (fd >= 0) {
            held_fds.push_back(fd);
        }
    }
    CHECK(!held_fds.empty());

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;

    auto start = std::chrono::steady_clock::now();
    CHECK(!backend.start(cfg, [](cas::TelemetryEvent) {}));
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(500));

    struct stat st {};
    CHECK(lstat(socket_path.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));

    for (int fd : held_fds) {
        close(fd);
    }
    close(live_fd);
    unlink(socket_path.c_str());
    remove_socket_tree(socket_path);
}

static void test_stop_preserves_replacement_socket() {
    std::string socket_path = make_socket_path("replacement");

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [](cas::TelemetryEvent) {}));

    CHECK(unlink(socket_path.c_str()) == 0);
    int replacement_fd = bind_listening_socket(socket_path);

    backend.stop();

    struct stat st {};
    CHECK(lstat(socket_path.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    int client_fd = -1;
    CHECK(try_connect_client(socket_path, client_fd));
    close(client_fd);
    close(replacement_fd);
    unlink(socket_path.c_str());
    remove_socket_tree(socket_path);
}

static void test_message_emits_event_and_allow_verdict() {
    std::string socket_path = make_socket_path("event");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Write);
    msg.pid = 1234;
    msg.bytes = 42;
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-event.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    CHECK(read_verdict(fd, verdict));
    CHECK(verdict == static_cast<char>(cas::Verdict::Allow));
    CHECK(wait_for_event_count(events, events_mutex, 1, 2000));

    {
        std::lock_guard<std::mutex> lock(events_mutex);
        CHECK(events.size() == 1);
        CHECK(events[0].backend == "ldpreload");
        CHECK(events[0].op == cas::OpType::Write);
        CHECK(events[0].verdict == cas::Verdict::Allow);
        CHECK(events[0].pid == static_cast<uint32_t>(getpid()));
        CHECK(events[0].uid == static_cast<uint32_t>(getuid()));
        CHECK(events[0].gid == static_cast<uint32_t>(getgid()));
        CHECK(events[0].bytes == 42);
        CHECK(events[0].path == "/tmp/agentvfs-ldpreload-event.txt");
    }

    close(fd);
    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_policy_soft_watch_sets_event_and_verdict() {
    std::string socket_path = make_socket_path("policy");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    cas::PolicyRules rules{};
    rules.rules.push_back(cas::PolicyRule{
        "/tmp/agentvfs-ldpreload-policy.txt",
        op_bit(cas::OpType::Write),
    });
    CHECK(backend.install_policy(rules));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Write);
    msg.pid = 9999;
    msg.bytes = 9;
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-policy.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    CHECK(read_verdict(fd, verdict));
    CHECK(verdict == static_cast<char>(cas::Verdict::SoftWatch));
    CHECK(wait_for_event_count(events, events_mutex, 1, 2000));

    {
        std::lock_guard<std::mutex> lock(events_mutex);
        CHECK(events.size() == 1);
        CHECK(events[0].verdict == cas::Verdict::SoftWatch);
    }

    close(fd);
    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_peer_credentials_override_wire_pid() {
    std::string socket_path = make_socket_path("peercred");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Open);
    msg.pid = 424242;
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-peercred.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    CHECK(read_verdict(fd, verdict));
    CHECK(wait_for_event_count(events, events_mutex, 1, 2000));

    {
        std::lock_guard<std::mutex> lock(events_mutex);
        CHECK(events.size() == 1);
        CHECK(events[0].pid == static_cast<uint32_t>(getpid()));
        CHECK(events[0].uid == static_cast<uint32_t>(getuid()));
        CHECK(events[0].gid == static_cast<uint32_t>(getgid()));
    }

    close(fd);
    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_partial_message_does_not_emit_event_or_verdict() {
    std::string socket_path = make_socket_path("partial");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Read);
    msg.pid = 55;
    write_full(fd, &msg, sizeof(msg) / 2);
    shutdown(fd, SHUT_WR);

    char verdict = 0;
    CHECK(!read_verdict(fd, verdict, 200));
    CHECK(!wait_for_event_count(events, events_mutex, 1, 200));
    close(fd);

    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_unsupported_wire_ops_are_dropped() {
    std::string socket_path = make_socket_path("unsupported");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Unlink);
    msg.pid = static_cast<uint32_t>(getpid());
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-unsupported.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    CHECK(!read_verdict(fd, verdict, 200));
    CHECK(!wait_for_event_count(events, events_mutex, 1, 200));
    close(fd);

    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_version_mismatch_is_politely_refused() {
    std::string socket_path = make_socket_path("version-mismatch");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = static_cast<uint8_t>(
        cas::ldpreload_protocol::kCurrentVersion + 1);
    msg.op = static_cast<uint8_t>(cas::OpType::Write);
    msg.pid = static_cast<uint32_t>(getpid());
    msg.bytes = 1;
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-versionmismatch.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    // Daemon must drop the connection without sending a verdict and without
    // emitting an event, rather than crashing on a future protocol bump.
    CHECK(!read_verdict(fd, verdict, 200));
    CHECK(!wait_for_event_count(events, events_mutex, 1, 200));
    close(fd);

    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_truncation_flag_propagates_to_event_extra() {
    std::string socket_path = make_socket_path("truncate-flag");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.flags = cas::ldpreload_protocol::kFlagPathTruncated;
    msg.op = static_cast<uint8_t>(cas::OpType::Open);
    msg.pid = static_cast<uint32_t>(getpid());
    msg.bytes = 0;
    std::memset(msg.path, 'a', sizeof(msg.path) - 1);
    msg.path[sizeof(msg.path) - 1] = '\0';
    write_full(fd, &msg, sizeof(msg));

    char verdict = 0;
    CHECK(read_verdict(fd, verdict));
    CHECK(verdict == static_cast<char>(cas::Verdict::Allow));
    CHECK(wait_for_event_count(events, events_mutex, 1, 2000));

    {
        std::lock_guard<std::mutex> lock(events_mutex);
        CHECK(events.size() == 1);
        bool found = false;
        for (const auto& kv : events[0].extra) {
            if (kv.first == "path_truncated") {
                CHECK(kv.second == "1");
                found = true;
            }
        }
        CHECK(found);
    }

    close(fd);
    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_idle_partial_client_expires_without_stop() {
    std::string socket_path = make_socket_path("idle");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Read);
    msg.pid = 100;
    write_full(fd, &msg, sizeof(msg) / 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    (void)send(fd, &msg, sizeof(msg), MSG_NOSIGNAL);

    char verdict = 0;
    CHECK(!read_verdict(fd, verdict, 100));
    CHECK(!wait_for_event_count(events, events_mutex, 1, 100));
    close(fd);

    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_many_sequential_messages_do_not_hang() {
    std::string socket_path = make_socket_path("many");
    std::vector<cas::TelemetryEvent> events;
    std::mutex events_mutex;

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent ev) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(std::move(ev));
    }));

    constexpr int kMessages = 200;
    for (int i = 0; i < kMessages; ++i) {
        int fd = connect_client(socket_path);
        PreloadMsg msg {};
        msg.version = cas::ldpreload_protocol::kCurrentVersion;
        msg.op = static_cast<uint8_t>(cas::OpType::Write);
        msg.pid = static_cast<uint32_t>(i);
        msg.bytes = static_cast<uint64_t>(i + 1);
        std::snprintf(msg.path, sizeof(msg.path),
                      "/tmp/agentvfs-ldpreload-many-%d.txt", i);
        write_full(fd, &msg, sizeof(msg));

        char verdict = 0;
        CHECK(read_verdict(fd, verdict));
        CHECK(verdict == static_cast<char>(cas::Verdict::Allow));
        close(fd);
    }

    CHECK(wait_for_event_count(events, events_mutex, kMessages, 2000));
    backend.stop();
    remove_socket_tree(socket_path);
}

static void test_stop_quiesces_callbacks() {
    std::string socket_path = make_socket_path("quiesce");
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_entered = false;
    bool release_callback = false;
    std::atomic<bool> stop_returned{false};
    std::atomic<bool> after_stop{false};
    std::atomic<int> callbacks_started{0};
    std::atomic<int> callbacks_after_stop{0};

    cas::LdpreloadBackend backend;
    cas::BackendConfig cfg{};
    cfg.params["socket"] = socket_path;
    CHECK(backend.start(cfg, [&](cas::TelemetryEvent) {
        ++callbacks_started;
        {
            std::unique_lock<std::mutex> lock(callback_mutex);
            callback_entered = true;
            callback_cv.notify_all();
            callback_cv.wait(lock, [&] { return release_callback; });
        }
        if (after_stop.load()) {
            ++callbacks_after_stop;
        }
    }));

    int fd = connect_client(socket_path);
    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.op = static_cast<uint8_t>(cas::OpType::Read);
    msg.pid = 77;
    msg.bytes = 5;
    std::strncpy(msg.path, "/tmp/agentvfs-ldpreload-quiesce.txt",
                 sizeof(msg.path) - 1);
    write_full(fd, &msg, sizeof(msg));

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        CHECK(callback_cv.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return callback_entered; }));
    }

    std::thread stop_thread([&] {
        backend.stop();
        stop_returned.store(true);
        callback_cv.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!stop_returned.load());

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_cv.notify_all();
    stop_thread.join();

    CHECK(stop_returned.load());
    CHECK(callbacks_started.load() == 1);
    after_stop.store(true);

    char verdict = 0;
    (void)read_verdict(fd, verdict, 100);
    (void)send(fd, &msg, sizeof(msg), MSG_NOSIGNAL);
    close(fd);

    int late_fd = -1;
    CHECK(!try_connect_client(socket_path, late_fd));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(callbacks_started.load() == 1);
    CHECK(callbacks_after_stop.load() == 0);
    remove_socket_tree(socket_path);
}

int main() {
    test_name_and_capabilities();
    test_default_socket_path_uses_xdg_runtime_dir_when_set();
    test_session_and_policy_hooks_accept_current_contract();
    test_start_stop_cleans_socket_and_is_idempotent();
    test_start_hardens_socket_permissions();
    test_start_rejects_too_long_socket_path();
    test_start_preserves_existing_non_socket_path();
    test_start_preserves_live_existing_socket();
    test_start_preserves_live_socket_with_full_backlog();
    test_stop_preserves_replacement_socket();
    test_message_emits_event_and_allow_verdict();
    test_policy_soft_watch_sets_event_and_verdict();
    test_peer_credentials_override_wire_pid();
    test_partial_message_does_not_emit_event_or_verdict();
    test_unsupported_wire_ops_are_dropped();
    test_version_mismatch_is_politely_refused();
    test_truncation_flag_propagates_to_event_extra();
    test_idle_partial_client_expires_without_stop();
    test_many_sequential_messages_do_not_hang();
    test_stop_quiesces_callbacks();
    std::fprintf(stderr, "test_ldpreload_backend: all tests passed\n");
    return 0;
}
