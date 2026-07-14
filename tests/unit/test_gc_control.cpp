// gc.run / gc.verify control commands (2026-07-13 mem-and-gc design, Task 8).
//
// Drives the line-oriented control protocol DIRECTLY via
// cas::control_protocol::dispatch against a constructed Daemon (mkdtemp
// store, daemon.initialize()). No control socket is started. Style mirrors
// test_stats_memory.cpp: a TestDaemon fixture, tiny flat-JSON checks, REQUIRE
// macros. The fixture is copied verbatim from test_stats_memory.cpp /
// test_agent_state_control.cpp (file-local statics, not a shared header).

#include "control_protocol.h"
#include "commit.h"
#include "daemon.h"
#include "hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// ---------------------------------------------------------------------------
// Test fixture (copied from test_stats_memory.cpp).
// ---------------------------------------------------------------------------

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-gc-control-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
    REQUIRE(path != nullptr);
    return std::string(path);
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else std::remove(child.c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}

struct TestDaemon {
    std::string root;
    std::string source;
    std::string mount;
    std::string store_root;
    Daemon daemon;

    explicit TestDaemon(const char* suffix)
        : root(make_tmp_dir(suffix))
        , source(root + "/src")
        , mount(root + "/mnt")
        , store_root(root + "/store")
        , daemon(source, mount, store_root) {
        REQUIRE(mkdir(source.c_str(), 0755) == 0);
        REQUIRE(mkdir(mount.c_str(), 0755) == 0);
        REQUIRE(daemon.initialize());
    }

    ~TestDaemon() { remove_dir_recursive(root); }
};

// ---------------------------------------------------------------------------
// Drive gc.run / gc.verify through the dispatch layer end to end. Verifies:
//  - dry_run reports the orphan but leaves it on disk,
//  - a real gc.run sweeps it,
//  - gc.verify reports ok / 0 missing,
//  - a follow-up dry_run on the now-clean store sweeps nothing.
// ---------------------------------------------------------------------------

static void test_gc_run_verify_dispatch() {
    TestDaemon td("gcrun");
    // Two checkpoints establish a branch ref the marker walks. The source
    // dir is empty, so both commits reference an empty tree.
    REQUIRE(control_protocol::dispatch(td.daemon, "checkpoint one")
            .find("\"ok\":true") != std::string::npos);
    REQUIRE(control_protocol::dispatch(td.daemon, "checkpoint two")
            .find("\"ok\":true") != std::string::npos);

    // Publish a genuinely orphan blob (drained, so not pending) and age it
    // past the 2-second fence.
    Hash orphan = td.daemon.store().write_blob(
        reinterpret_cast<const uint8_t*>("orphZ"), 5);
    (void)td.daemon.store().drain_pending();
    struct timespec ts[2];
    ts[0].tv_sec = std::time(nullptr) - 10; ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    REQUIRE(utimensat(AT_FDCWD, td.daemon.store().object_path(orphan).c_str(),
                      ts, 0) == 0);

    std::string dry = control_protocol::dispatch(td.daemon, "gc.run dry_run=1");
    REQUIRE(dry.find("\"ok\":true") != std::string::npos);
    REQUIRE(dry.find("\"dry_run\":true") != std::string::npos);
    REQUIRE(dry.find("\"swept_objects\":1") != std::string::npos);
    REQUIRE(td.daemon.store().object_exists(orphan));   // dry_run kept it

    std::string real = control_protocol::dispatch(td.daemon, "gc.run");
    REQUIRE(real.find("\"ok\":true") != std::string::npos);
    REQUIRE(real.find("\"dry_run\":false") != std::string::npos);
    REQUIRE(real.find("\"swept_objects\":1") != std::string::npos);
    REQUIRE(!td.daemon.store().object_exists(orphan));  // swept

    std::string verify = control_protocol::dispatch(td.daemon, "gc.verify");
    REQUIRE(verify.find("\"ok\":true") != std::string::npos);
    REQUIRE(verify.find("\"missing_objects\":0") != std::string::npos);
    REQUIRE(verify.find("\"missing\":[]") != std::string::npos);
    REQUIRE(verify.find("\"sweep_errors\":0") != std::string::npos);

    std::string again = control_protocol::dispatch(td.daemon, "gc.run dry_run=1");
    REQUIRE(again.find("\"swept_objects\":0") != std::string::npos);   // store clean

    std::printf("  PASS test_gc_run_verify_dispatch\n");
}

static void test_gc_options_are_strictly_validated() {
    TestDaemon td("strict-options");
    const char* invalid[] = {
        "gc.run keep_last=",
        "gc.run keep_last=0",
        "gc.run keep_last=-1",
        "gc.run keep_last=1junk",
        "gc.run keep_last=4294967296",
        "gc.run keep_label=",
        "gc.run dry_run=",
        "gc.run dry_run=2",
        "gc.run dry_run=true",
    };
    for (const char* command : invalid) {
        std::string response = control_protocol::dispatch(td.daemon, command);
        REQUIRE(response.find("\"ok\":false") != std::string::npos);
        REQUIRE(response.find("\"error\":") != std::string::npos);
    }

    std::string valid = control_protocol::dispatch(
        td.daemon, "gc.run keep_last=1 keep_label=pin dry_run=0");
    REQUIRE(valid.find("\"ok\":true") != std::string::npos);
    REQUIRE(valid.find("\"dry_run\":false") != std::string::npos);
    std::printf("  PASS test_gc_options_are_strictly_validated\n");
}

static void test_gc_verify_reports_missing_hashes() {
    TestDaemon td("missing-list");
    Hash missing = td.daemon.store().write_blob(
        reinterpret_cast<const uint8_t*>("missing"), 7);
    REQUIRE(!(missing == ZERO_HASH));
    // pending_snapshot makes this a live GC root; removing the file makes
    // verify report the exact missing id rather than failing during traversal.
    REQUIRE(std::remove(td.daemon.store().object_path(missing).c_str()) == 0);

    std::string response = control_protocol::dispatch(td.daemon, "gc.verify");
    REQUIRE(response.find("\"ok\":false") != std::string::npos);
    REQUIRE(response.find("\"missing_objects\":1") != std::string::npos);
    REQUIRE(response.find("\"missing\":[\"") != std::string::npos);
    REQUIRE(response.find(hash_to_hex(missing)) != std::string::npos);
    std::printf("  PASS test_gc_verify_reports_missing_hashes\n");
}

static void test_gc_verify_reports_missing_traversal_object() {
    TestDaemon td("missing-tree");
    Hash head;
    REQUIRE(td.daemon.refs().read_main(head));
    std::vector<uint8_t> body;
    REQUIRE(td.daemon.store().read_commit(head, body));
    CommitData commit;
    REQUIRE(deserialize_commit(body, commit));
    REQUIRE(std::remove(
        td.daemon.store().object_path(commit.tree_hash).c_str()) == 0);

    std::string response = control_protocol::dispatch(td.daemon, "gc.verify");
    REQUIRE(response.find("\"ok\":false") != std::string::npos);
    REQUIRE(response.find("\"missing_objects\":1") != std::string::npos);
    REQUIRE(response.find(hash_to_hex(commit.tree_hash)) != std::string::npos);
    std::printf("  PASS test_gc_verify_reports_missing_traversal_object\n");
}

int main() {
    std::printf("test_gc_control:\n");
    test_gc_run_verify_dispatch();
    test_gc_options_are_strictly_validated();
    test_gc_verify_reports_missing_hashes();
    test_gc_verify_reports_missing_traversal_object();
    std::printf("PASS test_gc_control\n");
    return 0;
}
