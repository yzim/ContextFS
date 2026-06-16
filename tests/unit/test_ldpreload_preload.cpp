#include "backends/ldpreload_protocol.h"
#include "telemetry_event.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CAS_PRELOAD_LIB
#define CAS_PRELOAD_LIB "./libcas_preload.so"
#endif

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

using PreloadOpenFn = int (*)(const char*, int, ...);
using PreloadOpenAtFn = int (*)(int, const char*, int, ...);
using PreloadStatFn = int (*)(const char*, struct stat*);
using PreloadTruncateFn = int (*)(const char*, off_t);
using PreloadWriteFn = ssize_t (*)(int, const void*, size_t);

static std::string make_temp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-preload-") + suffix +
                        "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    CHECK(dir != nullptr);
    return std::string(dir);
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

static void read_full(int fd, void* data, size_t len) {
    auto* cursor = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = read(fd, cursor, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        CHECK(n > 0);
        cursor += n;
        len -= static_cast<size_t>(n);
    }
}

class VerdictServer {
public:
    VerdictServer(const std::string& socket_path, cas::Verdict verdict)
        : socket_path_(socket_path), verdict_(verdict) {
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        CHECK(socket_path_.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);
        unlink(socket_path_.c_str());
        CHECK(bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) == 0);
        CHECK(listen(listen_fd_, 1) == 0);

        thread_ = std::thread([this] { run(); });
    }

    ~VerdictServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
        }
        unlink(socket_path_.c_str());
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    const PreloadMsg& message() const { return msg_; }
    bool saw_message() const { return saw_message_.load(); }

private:
    void run() {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        CHECK(client_fd >= 0);
        read_full(client_fd, &msg_, sizeof(msg_));
        saw_message_.store(true);
        uint8_t verdict = static_cast<uint8_t>(verdict_);
        write_full(client_fd, &verdict, sizeof(verdict));
        close(client_fd);
    }

    std::string socket_path_;
    cas::Verdict verdict_;
    int listen_fd_ = -1;
    std::thread thread_;
    PreloadMsg msg_ {};
    std::atomic<bool> saw_message_{false};
};

class TimedVerdictServer {
public:
    explicit TimedVerdictServer(const std::string& socket_path)
        : socket_path_(socket_path) {
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        CHECK(socket_path_.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);
        unlink(socket_path_.c_str());
        CHECK(bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) == 0);
        CHECK(listen(listen_fd_, 1) == 0);

        thread_ = std::thread([this] { run(); });
    }

    ~TimedVerdictServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
        }
        unlink(socket_path_.c_str());
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool saw_message() const { return saw_message_.load(); }

private:
    void run() {
        struct pollfd pfd {};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        int ready = poll(&pfd, 1, 300);
        CHECK(ready >= 0);
        if (ready == 0) {
            return;
        }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        CHECK(client_fd >= 0);
        PreloadMsg msg {};
        read_full(client_fd, &msg, sizeof(msg));
        saw_message_.store(true);
        uint8_t verdict = static_cast<uint8_t>(cas::Verdict::Allow);
        write_full(client_fd, &verdict, sizeof(verdict));
        close(client_fd);
    }

    std::string socket_path_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> saw_message_{false};
};

static void* load_symbol(void* handle, const char* name) {
    dlerror();
    void* sym = dlsym(handle, name);
    const char* err = dlerror();
    if (err != nullptr) {
        std::fprintf(stderr, "dlsym(%s) failed: %s\n", name, err);
    }
    CHECK(err == nullptr);
    CHECK(sym != nullptr);
    return sym;
}

static void test_open_deny_skips_real_open() {
    std::string dir = make_temp_dir("open-deny");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string target_path = dir + "/denied.txt";
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);

    VerdictServer server(socket_path, cas::Verdict::Deny);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));

    errno = 0;
    int fd = preload_open(target_path.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd == -1);
    CHECK(errno == EACCES);
    server.join();

    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Open));
    CHECK(server.message().bytes == 0);
    CHECK(std::string(server.message().path) == target_path);
    struct stat st {};
    CHECK(stat(target_path.c_str(), &st) != 0 && errno == ENOENT);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_write_deny_skips_real_write_and_reports_requested_count() {
    std::string dir = make_temp_dir("write-deny");
    std::string socket_path = dir + "/cas_preload.sock";
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);

    int pipe_fds[2] = {-1, -1};
    CHECK(pipe(pipe_fds) == 0);

    VerdictServer server(socket_path, cas::Verdict::Deny);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_write =
        reinterpret_cast<PreloadWriteFn>(load_symbol(handle, "write"));

    const char payload[] = "abcdef";
    errno = 0;
    ssize_t rc = preload_write(pipe_fds[1], payload, 5);
    CHECK(rc == -1);
    CHECK(errno == EACCES);
    server.join();

    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Write));
    CHECK(server.message().bytes == 5);

    char byte = 0;
    int flags = fcntl(pipe_fds[0], F_GETFL);
    CHECK(flags >= 0);
    CHECK(fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == 0);
    CHECK(read(pipe_fds[0], &byte, 1) < 0 && errno == EAGAIN);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_long_path_emits_truncated_telemetry() {
    // The probe must NOT silently skip long paths: it must emit an event with
    // kFlagPathTruncated set so the daemon records path_truncated="1" in
    // TelemetryEvent::extra. A silent skip would be a 16x evasion gap
    // (PATH_MAX=4096 vs the 256-byte wire field).
    std::string dir = make_temp_dir("long-path");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string long_path = dir + "/" + std::string(260, 'a');
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));

    errno = 0;
    int fd = preload_open(long_path.c_str(), O_RDONLY);
    // Real libc still returns ENOENT (or similar) because the underlying
    // path doesn't exist, but the probe must emit telemetry first.
    CHECK(fd == -1);
    server.join();
    CHECK(server.saw_message());
    CHECK(server.message().version ==
          cas::ldpreload_protocol::kCurrentVersion);
    CHECK((server.message().flags &
           cas::ldpreload_protocol::kFlagPathTruncated) != 0);
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Open));
    // Stored path is NUL-terminated and shorter than the original.
    CHECK(std::strlen(server.message().path) <
          cas::ldpreload_protocol::kPathSize);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_null_path_hooks_defer_to_real_libc_errors() {
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));
    auto preload_openat =
        reinterpret_cast<PreloadOpenAtFn>(load_symbol(handle, "openat"));
    auto preload_truncate =
        reinterpret_cast<PreloadTruncateFn>(load_symbol(handle, "truncate"));
    auto preload_stat =
        reinterpret_cast<PreloadStatFn>(load_symbol(handle, "stat"));
    auto preload_lstat =
        reinterpret_cast<PreloadStatFn>(load_symbol(handle, "lstat"));

    errno = 0;
    CHECK(preload_open(nullptr, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_openat(AT_FDCWD, nullptr, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_truncate(nullptr, 0) == -1);
    CHECK(errno == EFAULT);

    struct stat st {};
    errno = 0;
    CHECK(preload_stat(nullptr, &st) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_lstat(nullptr, &st) == -1);
    CHECK(errno == EFAULT);

    dlclose(handle);
}

static void test_invalid_path_hooks_defer_to_real_libc_errors() {
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));
    auto preload_open64 =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open64"));
    auto preload_openat =
        reinterpret_cast<PreloadOpenAtFn>(load_symbol(handle, "openat"));
    auto preload_openat64 =
        reinterpret_cast<PreloadOpenAtFn>(load_symbol(handle, "openat64"));
    auto preload_truncate =
        reinterpret_cast<PreloadTruncateFn>(load_symbol(handle, "truncate"));
    auto preload_stat =
        reinterpret_cast<PreloadStatFn>(load_symbol(handle, "stat"));
    auto preload_lstat =
        reinterpret_cast<PreloadStatFn>(load_symbol(handle, "lstat"));

    const char* invalid_path = reinterpret_cast<const char*>(1);

    errno = 0;
    CHECK(preload_open(invalid_path, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_open64(invalid_path, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_openat(AT_FDCWD, invalid_path, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_openat64(AT_FDCWD, invalid_path, O_RDONLY) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_truncate(invalid_path, 0) == -1);
    CHECK(errno == EFAULT);

    struct stat st {};
    errno = 0;
    CHECK(preload_stat(invalid_path, &st) == -1);
    CHECK(errno == EFAULT);

    errno = 0;
    CHECK(preload_lstat(invalid_path, &st) == -1);
    CHECK(errno == EFAULT);

    dlclose(handle);
}

static void test_open_relative_path_reports_cwd_resolved_path() {
    std::string dir = make_temp_dir("open-relative");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/relative.txt";
    char old_cwd[4096];
    CHECK(getcwd(old_cwd, sizeof(old_cwd)) != nullptr);
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);
    CHECK(chdir(dir.c_str()) == 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));

    int fd = preload_open("relative.txt", O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);
    server.join();

    CHECK(chdir(old_cwd) == 0);
    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Open));
    CHECK(std::string(server.message().path) == child_path);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_openat_relative_path_reports_resolved_path() {
    std::string dir = make_temp_dir("openat");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/child.txt";
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);

    int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    CHECK(dir_fd >= 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_openat =
        reinterpret_cast<PreloadOpenAtFn>(load_symbol(handle, "openat"));

    int fd = preload_openat(dir_fd, "child.txt", O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);
    server.join();

    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Open));
    CHECK(std::string(server.message().path) == child_path);

    close(dir_fd);
    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_openat_at_fdcwd_reports_cwd_resolved_path() {
    std::string dir = make_temp_dir("openat-cwd");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/cwdchild.txt";
    char old_cwd[4096];
    CHECK(getcwd(old_cwd, sizeof(old_cwd)) != nullptr);
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);
    CHECK(chdir(dir.c_str()) == 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_openat =
        reinterpret_cast<PreloadOpenAtFn>(load_symbol(handle, "openat"));

    int fd = preload_openat(AT_FDCWD, "cwdchild.txt",
                            O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);
    server.join();

    CHECK(chdir(old_cwd) == 0);
    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Open));
    CHECK(std::string(server.message().path) == child_path);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_stat_relative_path_reports_cwd_resolved_path() {
    std::string dir = make_temp_dir("stat-relative");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/stat.txt";
    int fd = open(child_path.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);

    char old_cwd[4096];
    CHECK(getcwd(old_cwd, sizeof(old_cwd)) != nullptr);
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);
    CHECK(chdir(dir.c_str()) == 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_stat =
        reinterpret_cast<PreloadStatFn>(load_symbol(handle, "stat"));

    struct stat st {};
    CHECK(preload_stat("stat.txt", &st) == 0);
    server.join();

    CHECK(chdir(old_cwd) == 0);
    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Stat));
    CHECK(std::string(server.message().path) == child_path);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

static void test_truncate_relative_path_reports_cwd_resolved_path() {
    std::string dir = make_temp_dir("truncate-relative");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/truncate.txt";
    int fd = open(child_path.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);

    char old_cwd[4096];
    CHECK(getcwd(old_cwd, sizeof(old_cwd)) != nullptr);
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);
    CHECK(chdir(dir.c_str()) == 0);

    VerdictServer server(socket_path, cas::Verdict::Allow);
    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_truncate =
        reinterpret_cast<PreloadTruncateFn>(load_symbol(handle, "truncate"));

    CHECK(preload_truncate("truncate.txt", 0) == 0);
    server.join();

    CHECK(chdir(old_cwd) == 0);
    CHECK(server.saw_message());
    CHECK(server.message().op == static_cast<uint8_t>(cas::OpType::Truncate));
    CHECK(std::string(server.message().path) == child_path);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

using PreloadDefaultSocketFn = char* (*)();

static void test_client_default_socket_path_is_per_user() {
    // Save environment.
    std::string saved_xdg;
    bool had_xdg = false;
    if (const char* prev = std::getenv("XDG_RUNTIME_DIR")) {
        saved_xdg = prev;
        had_xdg = true;
    }
    std::string saved_sock;
    bool had_sock = false;
    if (const char* prev = std::getenv("CAS_PRELOAD_SOCKET")) {
        saved_sock = prev;
        had_sock = true;
    }
    CHECK(unsetenv("CAS_PRELOAD_SOCKET") == 0);

    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto get_default = reinterpret_cast<PreloadDefaultSocketFn>(
        load_symbol(handle, "cas_preload_default_socket_path_for_test"));

    // XDG_RUNTIME_DIR set and existing -> path under it.
    std::string dir_template = "/tmp/agentvfs-preload-xdg-XXXXXX";
    std::vector<char> buf(dir_template.begin(), dir_template.end());
    buf.push_back('\0');
    char* mk = mkdtemp(buf.data());
    CHECK(mk != nullptr);
    std::string xdg = mk;
    CHECK(setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1) == 0);
    char* p1 = get_default();
    CHECK(p1 != nullptr);
    CHECK(std::string(p1) == xdg + "/cas_preload.sock");
    free(p1);

    // XDG_RUNTIME_DIR unset -> /tmp/cas_preload-${UID}.sock.
    CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
    char* p2 = get_default();
    CHECK(p2 != nullptr);
    std::string s2(p2);
    free(p2);
    CHECK(s2.find("/tmp/cas_preload-") == 0);
    CHECK(s2 != "/tmp/cas_preload.sock");
    CHECK(s2.find(std::to_string(geteuid())) != std::string::npos);

    dlclose(handle);
    rmdir(xdg.c_str());
    if (had_xdg) {
        setenv("XDG_RUNTIME_DIR", saved_xdg.c_str(), 1);
    }
    if (had_sock) {
        setenv("CAS_PRELOAD_SOCKET", saved_sock.c_str(), 1);
    }
}

// Verifies the child of a fork() can call interposed hooks without
// deadlocking on the parent's inherited pthread_once_t. Without the
// pthread_atfork reset, a fork() racing with init_real_functions could
// leave init_once flagged "in progress" in the child, causing the child's
// next pthread_once call to deadlock forever.
static void test_fork_child_can_reinit_hooks() {
    std::string dir = make_temp_dir("fork-reinit");
    std::string socket_path = dir + "/cas_preload.sock";
    std::string child_path = dir + "/forkchild.txt";
    CHECK(setenv("CAS_PRELOAD_SOCKET", socket_path.c_str(), 1) == 0);

    void* handle = dlopen(CAS_PRELOAD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(handle != nullptr);
    auto preload_open =
        reinterpret_cast<PreloadOpenFn>(load_symbol(handle, "open"));

    // Touch the hooks once in the parent so init_once is set.
    {
        VerdictServer warmup(socket_path, cas::Verdict::Allow);
        int fd = preload_open((dir + "/warmup.txt").c_str(),
                              O_CREAT | O_WRONLY, 0600);
        CHECK(fd >= 0);
        close(fd);
        warmup.join();
        unlink((dir + "/warmup.txt").c_str());
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child: must be able to call hooks without deadlocking on parent's
        // inherited pthread_once_t. Use a pipe-fd that already exists, no
        // socket needed.
        int fd = preload_open(child_path.c_str(),
                              O_CREAT | O_WRONLY, 0600);
        // Connect to nothing — preflight will fail-open, and real_open
        // will execute normally. Either outcome (fd>=0 or fd<0) is fine;
        // what matters is that we do not deadlock.
        if (fd >= 0) {
            close(fd);
        }
        _exit(0);
    }
    CHECK(pid > 0);

    int status = 0;
    pid_t waited = 0;
    // Use alarm() in case of deadlock so the test fails loudly.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (waited != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        CHECK(!"child deadlocked after fork — pthread_atfork reset missing");
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    dlclose(handle);
    unsetenv("CAS_PRELOAD_SOCKET");
    unlink(child_path.c_str());
    unlink(socket_path.c_str());
    rmdir(dir.c_str());
}

} // namespace

int main() {
    test_open_deny_skips_real_open();
    test_write_deny_skips_real_write_and_reports_requested_count();
    test_long_path_emits_truncated_telemetry();
    test_null_path_hooks_defer_to_real_libc_errors();
    test_invalid_path_hooks_defer_to_real_libc_errors();
    test_open_relative_path_reports_cwd_resolved_path();
    test_openat_relative_path_reports_resolved_path();
    test_openat_at_fdcwd_reports_cwd_resolved_path();
    test_stat_relative_path_reports_cwd_resolved_path();
    test_truncate_relative_path_reports_cwd_resolved_path();
    test_client_default_socket_path_is_per_user();
    test_fork_child_can_reinit_hooks();
    std::fprintf(stderr, "test_ldpreload_preload: all tests passed\n");
    return 0;
}
