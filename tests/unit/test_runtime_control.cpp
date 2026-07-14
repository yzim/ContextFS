// Integration tests for the daemon-coupled runtime snapshot/restore flow
// (Task 3). These tests drive the control protocol DIRECTLY via
// cas::control_protocol::dispatch on std::threads that rendezvous through
// the RuntimeSupervisor. No control socket is started.
//
// The cooperative runtimes use REAL sleeper child processes so that the
// PosixRuntimeProcessController's process_alive / freeze / terminate
// operations are exercised end-to-end (template-alive checks for
// restore eligibility, and freeze-but-don't-kill for restore).

#include "control_protocol.h"
#include "daemon.h"
#include "commit.h"
#include "hash.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <utility>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// ---------------------------------------------------------------------------
// Test fixture (same style as test_branch_merge_daemon.cpp).
// ---------------------------------------------------------------------------

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-runtime-control-") + suffix + "-XXXXXX";
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
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
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

    ~TestDaemon() {
        remove_dir_recursive(root);
    }
};

// ---------------------------------------------------------------------------
// Sleeper process helpers.
// ---------------------------------------------------------------------------

// Fork a child that becomes its own process-group leader and pauses until
// signalled. fork() happens from the single test thread (all rendezvous
// threads are joined before any spawn), so the child runs in a
// single-threaded context.
static pid_t spawn_sleeper() {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Test helpers never write output. Do not keep CTest's capture pipes
        // alive if the parent exits early on a failed assertion.
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        // Child: own process group, clean handler for SIGTERM, then pause.
        setpgid(0, 0);
        struct sigaction sa;
        sa.sa_handler = [](int) { _exit(0); };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        for (;;) {
            pause();
        }
        _exit(0);
    }
    // Parent: win the setpgid race (child may have already done it).
    setpgid(pid, pid);
    return pid;
}

static void kill_and_reap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 40; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        if (r < 0 && errno == ECHILD) return;
        usleep(25000);
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
}

static bool process_exists(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

// Reap a child that another component (the daemon) has already signalled.
// Returns true if the child was reaped as dead within the deadline.
static bool reap_dead(pid_t pid, int deadline_ms) {
    if (pid <= 0) return true;
    for (int i = 0; i < deadline_ms / 25 + 1; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == ECHILD) return true;
        usleep(25000);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tiny JSON field extractors (the protocol emits flat JSON).
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

static long json_int(const std::string& s, const std::string& key, long dflt) {
    std::string needle = "\"" + key + "\":";
    auto p = s.find(needle);
    if (p == std::string::npos) return dflt;
    p += needle.size();
    char* end = nullptr;
    long v = std::strtol(s.c_str() + p, &end, 10);
    if (end == s.c_str() + p) return dflt;
    return v;
}

static bool json_ok(const std::string& s) {
    return s.find("\"ok\":true") != std::string::npos;
}

static bool is_hex_commit(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

#if defined(__linux__)

static PeerCredentials peer_for_pid(pid_t pid) {
    PeerCredentials peer;
    peer.available = true;
    peer.pid = static_cast<int64_t>(pid);
    peer.uid = static_cast<int64_t>(getuid());
    peer.gid = static_cast<int64_t>(getgid());
    return peer;
}

static std::pair<pid_t, pid_t> spawn_launcher_with_child() {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    pid_t launcher = fork();
    REQUIRE(launcher >= 0);
    if (launcher == 0) {
        close(pipefd[0]);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        pid_t child = fork();
        if (child < 0) _exit(127);
        if (child == 0) {
            close(pipefd[1]);
            setpgid(0, 0);
            struct sigaction sa;
            sa.sa_handler = [](int) { _exit(0); };
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGTERM, &sa, nullptr);
            sigaction(SIGINT, &sa, nullptr);
            for (;;) pause();
        }
        setpgid(child, child);
        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%ld\n", static_cast<long>(child));
        ssize_t wr = write(pipefd[1], buf, static_cast<size_t>(n));
        (void)wr;
        close(pipefd[1]);
        for (;;) pause();
    }
    close(pipefd[1]);
    char buf[64] = {};
    size_t used = 0;
    while (used + 1 < sizeof(buf)) {
        char ch = '\0';
        ssize_t n = read(pipefd[0], &ch, 1);
        if (n == 1) {
            if (ch == '\n') break;
            buf[used++] = ch;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(pipefd[0]);
    long child = std::strtol(buf, nullptr, 10);
    REQUIRE(child > 0);
    return {launcher, static_cast<pid_t>(child)};
}

#endif

// ---------------------------------------------------------------------------
// Snapshot rendezvous helper. Drives runtime.snapshot + runtime.boundary +
// runtime.template.ready across three threads, returns the parsed result.
// ---------------------------------------------------------------------------

struct SnapshotOutcome {
    bool ok = false;
    std::string union_state_id;
    std::string fs_commit;
    std::string template_id;
    std::string restore_eligibility;
    std::string raw_snapshot;
    std::string raw_boundary;
    std::string raw_ready;
};

static SnapshotOutcome do_snapshot(TestDaemon& env,
                                   const std::string& runtime_id,
                                   int64_t boundary_pid,
                                   pid_t template_pid,
                                   const std::string& token = "tok-rt") {
    Daemon& d = env.daemon;
    SnapshotOutcome out;

    std::string snap_line = "runtime.snapshot {\"runtime_id\":\"" +
        runtime_id + "\",\"boundary_kind\":\"manual\",\"timeout_ms\":5000}";

    std::string snap_response;
    std::thread snap_thread([&] {
        snap_response = control_protocol::dispatch(d, snap_line);
    });

    // Prove the snapshot is blocked at wait_for_boundary before the runtime
    // reports the boundary.
    for (int i = 0; i < 50 && snap_response.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(snap_response.empty());  // snapshot is parked waiting for boundary

    std::string boundary_line =
        "runtime.boundary {\"runtime_id\":\"" + runtime_id +
        "\",\"control_token\":\"" + token +
        "\",\"pid\":" + std::to_string(boundary_pid) +
        ",\"generation\":1,\"boundary_kind\":\"manual\"}";
    std::string boundary_response;
    std::thread boundary_thread([&] {
        boundary_response = control_protocol::dispatch(d, boundary_line);
    });
    boundary_thread.join();  // returns action:"snapshot" with template_id

    std::string template_id = json_str(boundary_response, "template_id");
    REQUIRE(!template_id.empty());

    std::string ready_line =
        "runtime.template.ready {\"runtime_id\":\"" + runtime_id +
        "\",\"control_token\":\"" + token +
        "\",\"template_id\":\"" + template_id +
        "\",\"template_pid\":" + std::to_string((long)template_pid) +
        ",\"template_process_group_id\":" + std::to_string((long)template_pid) +
        ",\"generation\":1}";
    std::string ready_response;
    std::thread ready_thread([&] {
        ready_response = control_protocol::dispatch(d, ready_line);
    });
    ready_thread.join();
    snap_thread.join();

    out.raw_snapshot = snap_response;
    out.raw_boundary = boundary_response;
    out.raw_ready = ready_response;
    out.ok = json_ok(snap_response);
    out.union_state_id = json_str(snap_response, "union_state_id");
    out.fs_commit = json_str(snap_response, "fs_commit");
    out.template_id = template_id;
    out.restore_eligibility = json_str(snap_response, "restore_eligibility");
    return out;
}

// ---------------------------------------------------------------------------
// Test 1: create + status.
// ---------------------------------------------------------------------------

static void test_create_and_status_generation_one() {
    TestDaemon env("create");
    Daemon& d = env.daemon;
    pid_t root_pid = spawn_sleeper();

    std::string r = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-test\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ","
           "\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-rt\"}");
    REQUIRE(json_ok(r));
    REQUIRE(json_str(r, "runtime_id") == "rt-test");
    REQUIRE(json_int(r, "generation", -1) == 1);

    std::string s = control_protocol::dispatch(
        d, "runtime.status {\"runtime_id\":\"rt-test\"}");
    REQUIRE(json_ok(s));
    REQUIRE(json_str(s, "runtime_id") == "rt-test");
    REQUIRE(json_str(s, "branch") == "main");
    REQUIRE(json_int(s, "generation", -1) == 1);

    // Unknown runtime must surface the precise error string.
    std::string s2 = control_protocol::dispatch(
        d, "runtime.status {\"runtime_id\":\"nope\"}");
    REQUIRE(s2.find("\"ok\":false") != std::string::npos);
    REQUIRE(s2.find("unknown runtime") != std::string::npos);

    kill_and_reap(root_pid);
    std::printf("  PASS test_create_and_status_generation_one\n");
}

// ---------------------------------------------------------------------------
// Test 2: multi-thread snapshot rendezvous -> boundary -> template.ready ->
//         union_state_id.
// ---------------------------------------------------------------------------

static void test_snapshot_boundary_template_ready_publishes_union_state() {
    TestDaemon env("snapshot");
    Daemon& d = env.daemon;

    pid_t template_pid = spawn_sleeper();
    pid_t root_pid = spawn_sleeper();

    // Put a blob so the branch has real content to checkpoint.
    auto main = d.main_branch();
    Hash blob = d.store().write_blob(
        reinterpret_cast<const uint8_t*>("snap"), 4);
    REQUIRE(blob != ZERO_HASH);
    main->wt.insert("/a.txt", {EntryKind::Blob, blob, 0100644});

    REQUIRE(control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-test\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ","
           "\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-rt\"}").find("\"ok\":true") != std::string::npos);

    SnapshotOutcome snap = do_snapshot(env, "rt-test", root_pid, template_pid);

    REQUIRE(snap.ok);
    REQUIRE(is_hex_commit(snap.union_state_id));
    REQUIRE(is_hex_commit(snap.fs_commit));
    REQUIRE(snap.template_id.compare(0, 5, "tmpl-") == 0);
    REQUIRE(json_ok(snap.raw_boundary));
    REQUIRE(snap.raw_boundary.find("\"action\":\"snapshot\"") != std::string::npos);
    REQUIRE(snap.raw_boundary.find("\"operation_id\"") != std::string::npos);
    REQUIRE(json_ok(snap.raw_ready));
    // Template is a live sleeper -> restorable while alive.
    REQUIRE(snap.restore_eligibility == "live_runtime_restorable");

    UnionRuntimeState us;
    std::string err;
    Hash union_hash;
    REQUIRE(hex_to_hash(snap.union_state_id.c_str(), union_hash));
    REQUIRE(read_union_runtime_state(d.store(), union_hash, us, err));
    REQUIRE(us.resource_manifest_ref == "inline:cooperative-process-group");
    REQUIRE(us.warnings.size() == 2);
    REQUIRE(us.warnings[0].kind == "process_group_descendants");
    REQUIRE(!us.warnings[0].blocker);
    REQUIRE(us.warnings[1].kind == "external_resources");
    REQUIRE(!us.warnings[1].blocker);
    REQUIRE(d.store().pending_count() == 0);

    kill_and_reap(template_pid);
    kill_and_reap(root_pid);

    std::printf("  PASS test_snapshot_boundary_template_ready_publishes_union_state\n");
}

// ---------------------------------------------------------------------------
// Test 3: restore records recovery commit, freezes-not-kills the old pgid,
//         rolls back, invalidates fhs, terminates old pgid only after
//         generation.ready.
// ---------------------------------------------------------------------------

static void test_restore_rolls_back_and_terminates_after_generation_ready() {
    TestDaemon env("restore");
    Daemon& d = env.daemon;

    pid_t root_pid = spawn_sleeper();       // active runtime process group
    pid_t template_pid = spawn_sleeper();   // parked template
    pid_t restored_pid = spawn_sleeper();   // new generation

    // Seed the branch with content.
    auto main = d.main_branch();
    Hash seed = d.store().write_blob(
        reinterpret_cast<const uint8_t*>("seed"), 4);
    REQUIRE(seed != ZERO_HASH);
    main->wt.insert("/seed.txt", {EntryKind::Blob, seed, 0100644});

    REQUIRE(json_ok(control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-test\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-rt\"}")));

    SnapshotOutcome snap = do_snapshot(env, "rt-test", root_pid, template_pid);
    REQUIRE(snap.ok);
    REQUIRE(is_hex_commit(snap.union_state_id));
    REQUIRE(is_hex_commit(snap.fs_commit));
    std::string snapshot_commit = snap.fs_commit;

    // Record the recovery commit: modify the working tree and checkpoint it so
    // the branch tip advances PAST the snapshot commit. Restore must capture
    // this as recovery_commit and roll the branch back to snapshot_commit.
    Hash changed = d.store().write_blob(
        reinterpret_cast<const uint8_t*>("changed"), 7);
    REQUIRE(changed != ZERO_HASH);
    main->wt.insert("/changed.txt", {EntryKind::Blob, changed, 0100644});
    auto mod_cp = d.checkpoint_branch_by_name("main", "pre-restore");
    REQUIRE(mod_cp.ok);
    Hash recovery_tip = mod_cp.commit_hash;
    REQUIRE(recovery_tip != ZERO_HASH);
    // Sanity: the branch tip now differs from the snapshot commit.
    REQUIRE(d.checkpoint_mgr().current_commit("main") == recovery_tip);

    // Open a (clean) file handle on main so we can observe invalidation.
    auto fh_state = std::make_unique<FhState>();
    fh_state->path = "/open.txt";
    fh_state->branch_id = main->branch_id;
    uint64_t fh = d.allocate_fh(std::move(fh_state));
    REQUIRE(d.get_fh(fh) != nullptr);
    REQUIRE(!d.get_fh(fh)->stale);

    // Dispatch restore on a thread: it freezes root_pid, records recovery,
    // rolls back, then blocks at wait_for_generation_ready.
    std::string restore_response;
    std::string restore_line =
        "runtime.restore {\"union_state_id\":\"" + snap.union_state_id + "\"}";
    std::thread restore_thread([&] {
        restore_response = control_protocol::dispatch(d, restore_line);
    });

    // Wait until restore has rolled the branch back to the snapshot commit
    // (and thus has already frozen the old pgid via begin_restore).
    bool rolled_back = false;
    for (int i = 0; i < 200; i++) {
        if (hash_to_hex(d.checkpoint_mgr().current_commit("main")) == snapshot_commit) {
            rolled_back = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(rolled_back);

    // The old active process group must be FROZEN, NOT KILLED at this point:
    // restore has not yet seen generation.ready.
    REQUIRE(process_exists(root_pid));

    // File handles for the branch must be invalidated by the rollback.
    REQUIRE(d.get_fh(fh) != nullptr);
    REQUIRE(d.get_fh(fh)->stale);

    // The rolled-back working tree no longer contains the post-snapshot edit.
    REQUIRE(!main->wt.lookup("/changed.txt").has_value());

    // The template's poll now returns action:"restore".
    std::string poll_response = control_protocol::dispatch(
        d, "runtime.template.poll {\"template_id\":\"" + snap.template_id +
           "\",\"control_token\":\"tok-rt\"}");
    REQUIRE(json_ok(poll_response));
    REQUIRE(poll_response.find("\"action\":\"restore\"") != std::string::npos);
    REQUIRE(json_int(poll_response, "target_generation", 0) == 2);

    // Advance the generation: this unblocks restore and retires root_pid.
    std::string gen_line =
        "runtime.generation.ready {\"runtime_id\":\"rt-test\","
        "\"control_token\":\"tok-rt\","
        "\"pid\":" + std::to_string((long)restored_pid) +
        ",\"active_process_group_id\":" + std::to_string((long)restored_pid) +
        ",\"generation\":2}";
    std::string gen_response = control_protocol::dispatch(d, gen_line);
    REQUIRE(json_ok(gen_response));

    restore_thread.join();
    REQUIRE(json_ok(restore_response));
    REQUIRE(json_str(restore_response, "template_id") == snap.template_id);
    REQUIRE(json_int(restore_response, "target_generation", 0) == 2);
    REQUIRE(json_str(restore_response, "fs_commit") == snapshot_commit);

    // The old active pgid is terminated only after generation.ready.
    REQUIRE(reap_dead(root_pid, 1000));
    REQUIRE(!process_exists(root_pid));

    // The recovery commit object is still readable in the store (recorded /
    // preserved by restore for the failure-recovery path).
    std::vector<uint8_t> body;
    REQUIRE(d.store().read_commit(recovery_tip, body));

    // The restored generation is now active in the supervisor.
    std::string status_after = control_protocol::dispatch(
        d, "runtime.status {\"runtime_id\":\"rt-test\"}");
    REQUIRE(json_int(status_after, "generation", 0) == 2);

    kill_and_reap(template_pid);
    kill_and_reap(restored_pid);

    std::printf("  PASS test_restore_rolls_back_and_terminates_after_generation_ready\n");
}

static void test_generation_ready_rejects_wrong_token_at_daemon_level() {
    TestDaemon env("gen-token");
    Daemon& d = env.daemon;

    pid_t root_pid = spawn_sleeper();
    pid_t template_pid = spawn_sleeper();
    pid_t restored_pid = spawn_sleeper();

    REQUIRE(json_ok(control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-gen\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-gen\"}")));

    SnapshotOutcome snap = do_snapshot(env, "rt-gen", root_pid, template_pid, "tok-gen");
    REQUIRE(snap.ok);

    std::string restore_response;
    std::thread restore_thread([&] {
        restore_response = control_protocol::dispatch(
            d, "runtime.restore {\"union_state_id\":\"" + snap.union_state_id +
               "\",\"timeout_ms\":500}");
    });

    bool restore_released = false;
    for (int i = 0; i < 200; i++) {
        std::string poll = control_protocol::dispatch(
            d, "runtime.template.poll {\"template_id\":\"" + snap.template_id +
               "\",\"control_token\":\"tok-gen\"}");
        if (poll.find("\"action\":\"restore\"") != std::string::npos) {
            restore_released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(restore_released);

    std::string bad_gen = control_protocol::dispatch(
        d, "runtime.generation.ready {\"runtime_id\":\"rt-gen\","
           "\"control_token\":\"wrong\",\"pid\":" + std::to_string((long)restored_pid) +
           ",\"active_process_group_id\":" + std::to_string((long)restored_pid) +
           ",\"generation\":2}");
    REQUIRE(bad_gen.find("\"ok\":false") != std::string::npos);
    REQUIRE(bad_gen.find("invalid control token") != std::string::npos);

    restore_thread.join();
    REQUIRE(restore_response.find("\"ok\":false") != std::string::npos);
    REQUIRE(restore_response.find("restore timeout") != std::string::npos);

    kill_and_reap(root_pid);
    kill_and_reap(template_pid);
    kill_and_reap(restored_pid);
    std::printf("  PASS test_generation_ready_rejects_wrong_token_at_daemon_level\n");
}

static void test_runtime_control_rejects_spoofed_tokens_and_invalid_registration() {
    TestDaemon env("tokens");
    Daemon& d = env.daemon;

    pid_t root_pid = spawn_sleeper();
    pid_t template_pid = spawn_sleeper();

    std::string no_token = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"missing-token\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true}");
    REQUIRE(no_token.find("\"ok\":false") != std::string::npos);
    REQUIRE(no_token.find("missing control token") != std::string::npos);

    std::string bad_branch = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"bad-branch\",\"branch\":\"missing\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-bad\"}");
    REQUIRE(bad_branch.find("\"ok\":false") != std::string::npos);
    REQUIRE(bad_branch.find("unknown branch") != std::string::npos);

    std::string bad_pid = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"bad-pid\",\"branch\":\"main\","
           "\"root_pid\":999999999,\"process_group_id\":999999999,"
           "\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-bad\"}");
    REQUIRE(bad_pid.find("\"ok\":false") != std::string::npos);
    REQUIRE(bad_pid.find("runtime process not live") != std::string::npos);

    REQUIRE(json_ok(control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-sec\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-sec\"}")));

    std::string snap_line =
        "runtime.snapshot {\"runtime_id\":\"rt-sec\",\"boundary_kind\":\"manual\","
        "\"timeout_ms\":5000}";
    std::string snap_response;
    std::thread snap_thread([&] {
        snap_response = control_protocol::dispatch(d, snap_line);
    });
    for (int i = 0; i < 50 && snap_response.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(snap_response.empty());

    std::string spoof_boundary = control_protocol::dispatch(
        d, "runtime.boundary {\"runtime_id\":\"rt-sec\","
           "\"control_token\":\"wrong\",\"pid\":" + std::to_string((long)root_pid) +
           ",\"generation\":1,\"boundary_kind\":\"manual\"}");
    REQUIRE(spoof_boundary.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_boundary.find("invalid control token") != std::string::npos);

    std::string boundary_response;
    std::thread boundary_thread([&] {
        boundary_response = control_protocol::dispatch(
            d, "runtime.boundary {\"runtime_id\":\"rt-sec\","
               "\"control_token\":\"tok-sec\",\"pid\":" +
               std::to_string((long)root_pid) +
               ",\"generation\":1,\"boundary_kind\":\"manual\"}");
    });
    boundary_thread.join();
    REQUIRE(json_ok(boundary_response));
    std::string template_id = json_str(boundary_response, "template_id");
    REQUIRE(!template_id.empty());

    std::string spoof_ready = control_protocol::dispatch(
        d, "runtime.template.ready {\"runtime_id\":\"rt-sec\","
           "\"control_token\":\"wrong\",\"template_id\":\"" + template_id +
           "\",\"template_pid\":" + std::to_string((long)template_pid) +
           ",\"template_process_group_id\":" + std::to_string((long)template_pid) +
           ",\"generation\":1}");
    REQUIRE(spoof_ready.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_ready.find("invalid control token") != std::string::npos);

    std::string ready_response;
    std::thread ready_thread([&] {
        ready_response = control_protocol::dispatch(
            d, "runtime.template.ready {\"runtime_id\":\"rt-sec\","
               "\"control_token\":\"tok-sec\",\"template_id\":\"" + template_id +
               "\",\"template_pid\":" + std::to_string((long)template_pid) +
               ",\"template_process_group_id\":" + std::to_string((long)template_pid) +
               ",\"generation\":1}");
    });
    ready_thread.join();
    snap_thread.join();
    REQUIRE(json_ok(ready_response));
    REQUIRE(json_ok(snap_response));

    std::string spoof_poll = control_protocol::dispatch(
        d, "runtime.template.poll {\"template_id\":\"" + template_id +
           "\",\"control_token\":\"wrong\"}");
    REQUIRE(spoof_poll.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_poll.find("invalid control token") != std::string::npos);

    kill_and_reap(root_pid);
    kill_and_reap(template_pid);
    std::printf("  PASS test_runtime_control_rejects_spoofed_tokens_and_invalid_registration\n");
}

#if defined(__linux__)

static void test_runtime_control_rejects_peer_spoofing() {
    TestDaemon env("peers");
    Daemon& d = env.daemon;

    auto [launcher_pid, root_pid] = spawn_launcher_with_child();
    PeerCredentials launcher_peer = peer_for_pid(launcher_pid);
    PeerCredentials wrong_peer = peer_for_pid(getpid());
    PeerCredentials root_peer = peer_for_pid(root_pid);

    std::string spoof_create = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-peer-bad\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-peer\"}",
        wrong_peer);
    REQUIRE(spoof_create.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_create.find("runtime launcher mismatch") != std::string::npos);

    std::string good_create = control_protocol::dispatch(
        d, "runtime.create {\"runtime_id\":\"rt-peer\",\"branch\":\"main\","
           "\"root_pid\":" + std::to_string((long)root_pid) +
           ",\"process_group_id\":" + std::to_string((long)root_pid) +
           ",\"command_ref\":\"fixture\",\"cooperative\":true,"
           "\"control_token\":\"tok-peer\"}",
        launcher_peer);
    REQUIRE(json_ok(good_create));

    std::string snap_line =
        "runtime.snapshot {\"runtime_id\":\"rt-peer\",\"boundary_kind\":\"manual\","
        "\"timeout_ms\":5000}";
    std::string snap_response;
    std::thread snap_thread([&] {
        snap_response = control_protocol::dispatch(d, snap_line);
    });
    for (int i = 0; i < 50 && snap_response.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(snap_response.empty());

    std::string spoof_boundary = control_protocol::dispatch(
        d, "runtime.boundary {\"runtime_id\":\"rt-peer\","
           "\"control_token\":\"tok-peer\",\"pid\":" + std::to_string((long)root_pid) +
           ",\"generation\":1,\"boundary_kind\":\"manual\"}",
        wrong_peer);
    REQUIRE(spoof_boundary.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_boundary.find("peer pid mismatch") != std::string::npos);

    std::string boundary_response;
    std::thread boundary_thread([&] {
        boundary_response = control_protocol::dispatch(
            d, "runtime.boundary {\"runtime_id\":\"rt-peer\","
               "\"control_token\":\"tok-peer\",\"pid\":" +
               std::to_string((long)root_pid) +
               ",\"generation\":1,\"boundary_kind\":\"manual\"}",
            root_peer);
    });
    boundary_thread.join();
    REQUIRE(json_ok(boundary_response));

    std::string template_id = json_str(boundary_response, "template_id");
    REQUIRE(!template_id.empty());
    pid_t template_pid = spawn_sleeper();
    PeerCredentials template_peer = peer_for_pid(template_pid);
    std::string spoof_ready = control_protocol::dispatch(
        d, "runtime.template.ready {\"runtime_id\":\"rt-peer\","
           "\"control_token\":\"tok-peer\",\"template_id\":\"" + template_id +
           "\",\"template_pid\":" + std::to_string((long)template_pid) +
           ",\"template_process_group_id\":" + std::to_string((long)template_pid) +
           ",\"generation\":1}",
        wrong_peer);
    REQUIRE(spoof_ready.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_ready.find("peer pid mismatch") != std::string::npos);

    std::string ready_response;
    std::thread ready_thread([&] {
        ready_response = control_protocol::dispatch(
            d, "runtime.template.ready {\"runtime_id\":\"rt-peer\","
               "\"control_token\":\"tok-peer\",\"template_id\":\"" + template_id +
               "\",\"template_pid\":" + std::to_string((long)template_pid) +
               ",\"template_process_group_id\":" + std::to_string((long)template_pid) +
               ",\"generation\":1}",
            template_peer);
    });
    ready_thread.join();
    snap_thread.join();
    REQUIRE(json_ok(ready_response));
    REQUIRE(json_ok(snap_response));

    std::string spoof_poll = control_protocol::dispatch(
        d, "runtime.template.poll {\"template_id\":\"" + template_id +
           "\",\"control_token\":\"tok-peer\"}",
        wrong_peer);
    REQUIRE(spoof_poll.find("\"ok\":false") != std::string::npos);
    REQUIRE(spoof_poll.find("peer pid mismatch") != std::string::npos);

    kill_and_reap(template_pid);
    kill_and_reap(root_pid);
    kill_and_reap(launcher_pid);
    std::printf("  PASS test_runtime_control_rejects_peer_spoofing\n");
}

#endif

int main() {
    std::printf("test_runtime_control:\n");
    test_create_and_status_generation_one();
    test_snapshot_boundary_template_ready_publishes_union_state();
    test_restore_rolls_back_and_terminates_after_generation_ready();
    test_generation_ready_rejects_wrong_token_at_daemon_level();
    test_runtime_control_rejects_spoofed_tokens_and_invalid_registration();
#if defined(__linux__)
    test_runtime_control_rejects_peer_spoofing();
#else
    std::printf("  SKIP test_runtime_control_rejects_peer_spoofing (Linux-only)\n");
#endif
    std::printf("PASS test_runtime_control\n");
    return 0;
}
