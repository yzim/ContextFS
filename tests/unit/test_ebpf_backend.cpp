#include "backends/ebpf_backend.h"
#include "daemon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-ebpf-backend-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
    CHECK(path != nullptr);
    return std::string(path);
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            remove_dir_recursive(child);
        } else {
            std::remove(child.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
}

struct TestDaemon {
    std::string root;
    std::string source;
    std::string mount;
    std::string store;
    cas::Daemon daemon;

    explicit TestDaemon(const char* suffix)
        : root(make_tmp_dir(suffix))
        , source(root + "/src")
        , mount(root + "/mnt")
        , store(root + "/store")
        , daemon(source, mount, store) {
        CHECK(mkdir(source.c_str(), 0755) == 0);
        CHECK(mkdir(mount.c_str(), 0755) == 0);
        CHECK(daemon.initialize());
    }

    ~TestDaemon() {
        remove_dir_recursive(root);
    }
};

static void test_name_and_capabilities() {
    TestDaemon env("caps");
    cas::EbpfBackend backend(env.daemon);

    cas::Capabilities caps = backend.capabilities();
    cas::OpMask expected = op_bit(cas::OpType::Read) |
                           op_bit(cas::OpType::Write) |
                           op_bit(cas::OpType::Unlink) |
                           op_bit(cas::OpType::Rename);

    CHECK(backend.name() == "ebpf");
    CHECK(caps.supported_ops == expected);
    CHECK(!caps.pre_op_verdicts);
    CHECK(caps.requires_cgroup);
    CHECK(caps.requires_root);
}

static void test_no_ebpf_build_start_fails_without_callbacks() {
    TestDaemon env("start-no-ebpf");
    cas::EbpfBackend backend(env.daemon);
    bool callback_called = false;

#ifndef AGENTVFS_EBPF
    CHECK(!backend.start(cas::BackendConfig{}, [&](cas::TelemetryEvent) {
        callback_called = true;
    }));
    CHECK(!backend.loader().available());
    backend.stop();
    CHECK(!callback_called);
#else
    (void)backend;
    (void)callback_called;
#endif
}

int main() {
    test_name_and_capabilities();
    test_no_ebpf_build_start_fails_without_callbacks();
    std::fprintf(stderr, "test_ebpf_backend: all tests passed\n");
    return 0;
}
