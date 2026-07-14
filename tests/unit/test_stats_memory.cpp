// stats.memory control command (2026-07-13 mem-and-gc design, Task 3).
//
// Drives the line-oriented control protocol DIRECTLY via
// cas::control_protocol::dispatch against a constructed Daemon (mkdtemp store,
// daemon.initialize()). No control socket is started. Style mirrors
// test_agent_state_control.cpp: a TestDaemon fixture, tiny flat-JSON
// extractors, REQUIRE macros. The fixture + extractors are copied verbatim
// from test_agent_state_control.cpp (they are file-local statics there, not a
// shared header).

#include "control_protocol.h"
#include "daemon.h"
#include "hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <chrono>
#include <future>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// ---------------------------------------------------------------------------
// Test fixture (copied from test_agent_state_control.cpp).
// ---------------------------------------------------------------------------

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-stats-memory-") + suffix + "-XXXXXX";
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
// Tiny flat-JSON field extractors (the protocol emits flat JSON).
// ---------------------------------------------------------------------------

static std::string json_str(const std::string& s, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto p = s.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    auto e = s.find('"', p);
    if (e == std::string::npos) return {};
    return s.substr(p, e - p);
}

static bool json_ok(const std::string& s) {
    return s.find("\"ok\":true") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Test 1: stats.memory returns the contract JSON shape.
// ---------------------------------------------------------------------------

static void test_stats_memory_shape() {
    TestDaemon td("stats");
    std::string resp = control_protocol::dispatch(td.daemon, "stats.memory");
    REQUIRE(resp.find("\"ok\":true") != std::string::npos);
    REQUIRE(resp.find("\"daemon_rss_kb\":") != std::string::npos);
    REQUIRE(resp.find("\"rss_kb\":") != std::string::npos);
    REQUIRE(resp.find("\"write_buffers\":") != std::string::npos);
    REQUIRE(resp.find("\"name\":\"main\"") != std::string::npos);
    REQUIRE(resp.find("\"base_entries\":") != std::string::npos);
    REQUIRE(resp.find("\"delta_tombstones\":") != std::string::npos);

    std::printf("  PASS test_stats_memory_shape\n");
}

// ---------------------------------------------------------------------------
// Test 2: stats.memory tracks working-tree mutations + branch sharing.
// ---------------------------------------------------------------------------

static void test_stats_track_mutations() {
    TestDaemon td("stats2");
    auto& wt = td.daemon.working_tree();
    wt.set_base(WorkingTree::EntryMap{});          // authoritative empty base
    wt.insert("/x.txt", {EntryKind::Blob, ZERO_HASH, 0100644});
    std::string resp = control_protocol::dispatch(td.daemon, "stats.memory");
    REQUIRE(resp.find("\"delta_entries\":1") != std::string::npos);
    // Branch create shares the base: main's base_shared_by rises to 2.
    // branch.create takes a JSON object (control_protocol parses with the
    // shared JSON-string extractor, not key=value tokens).
    std::string cb = control_protocol::dispatch(
        td.daemon, "branch.create {\"name\":\"twin\",\"from\":\"main\"}");
    REQUIRE(json_ok(cb));
    resp = control_protocol::dispatch(td.daemon, "stats.memory");
    REQUIRE(resp.find("\"base_shared_by\":2") != std::string::npos);
    REQUIRE(resp.find("\"name\":\"twin\"") != std::string::npos);

    std::printf("  PASS test_stats_track_mutations\n");
}

static void test_stats_waits_for_write_buffer_mutation_lock() {
    TestDaemon td("stats-lock");
    auto br = td.daemon.main_branch();

    auto state = std::make_unique<FhState>();
    state->path = "/buffered.txt";
    state->branch_id = br->branch_id;
    state->write_buf = std::make_unique<WriteBuffer>(ZERO_HASH, 0);
    uint64_t fh = td.daemon.allocate_fh(std::move(state));
    auto held = td.daemon.get_fh(fh);
    REQUIRE(held != nullptr);

    std::promise<void> dirty_ready;
    std::promise<void> release_writer;
    auto writer = std::async(std::launch::async, [&] {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        const uint8_t bytes[] = {'d', 'a', 't', 'a'};
        held->write_buf->write(0, bytes, sizeof(bytes));
        dirty_ready.set_value();
        release_writer.get_future().wait();
    });
    dirty_ready.get_future().wait();

    std::promise<void> snapshot_started;
    auto snapshot = std::async(std::launch::async, [&] {
        snapshot_started.set_value();
        return td.daemon.collect_memory_stats();
    });
    snapshot_started.get_future().wait();
    // stats.memory must use checkpoint_mu, the same lock that protects every
    // WriteBuffer mutation/replacement. Without it this returns immediately
    // and races the pointed-to state despite holding fh_table_mu_.
    REQUIRE(snapshot.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout);

    release_writer.set_value();
    writer.get();
    REQUIRE(snapshot.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    MemoryStats stats = snapshot.get();
    REQUIRE(stats.write_buffer_count == 1);
    REQUIRE(stats.write_buffer_dirty_bytes == 4);
    td.daemon.release_fh(fh);

    std::printf("  PASS test_stats_waits_for_write_buffer_mutation_lock\n");
}

int main() {
    std::printf("test_stats_memory:\n");
    test_stats_memory_shape();
    test_stats_track_mutations();
    test_stats_waits_for_write_buffer_mutation_lock();
    std::printf("PASS test_stats_memory\n");
    return 0;
}
