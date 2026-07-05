// Daemon-backed control-protocol tests for the agent-state API (Task 3).
//
// These tests drive the line-oriented control protocol DIRECTLY via
// cas::control_protocol::dispatch against a constructed Daemon (mkdtemp store,
// daemon.initialize()). No control socket is started, and no real cooperative
// runtime is required: the state.* commands operate purely on the CAS +
// AgentStateService, and the runtime.snapshot validation test asserts the
// agent_state_id guard fires BEFORE any runtime orchestration begins.
//
// The style mirrors test_runtime_control.cpp (minimal C++, no external test
// framework): a TestDaemon fixture, tiny flat-JSON extractors, REQUIRE macros.

#include "control_protocol.h"
#include "daemon.h"
#include "hash.h"
#include "runtime_state.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// ---------------------------------------------------------------------------
// Test fixture (same style as test_runtime_control.cpp).
// ---------------------------------------------------------------------------

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-state-control-") + suffix + "-XXXXXX";
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

static bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Dispatch helper: appends a state with the given JSON body (the part after
// `state.append `). Returns the full dispatch response.
static std::string do_append(TestDaemon& env, const std::string& json_body) {
    return control_protocol::dispatch(env.daemon, "state.append " + json_body);
}

// Append a logical-only root state (sync=false) and return its state_id.
static std::string append_logical_root(TestDaemon& env,
                                       const std::string& agent,
                                       const std::string& payload) {
    std::string body = std::string("{\"agent_id\":\"") + agent + "\","
        "\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"" + payload + "\","
        "\"sync\":false}";
    std::string r = do_append(env, body);
    REQUIRE(json_ok(r));
    std::string sid = json_str(r, "state_id");
    REQUIRE(is_hex64(sid));
    return sid;
}

// Append a durable state (sync=true) anchored at parent/snapshot, returning its
// state_id. Durable appends require readable parent + snapshot_base refs.
static std::string append_durable(TestDaemon& env,
                                  const std::string& agent,
                                  const std::string& parent_id,
                                  const std::string& snapshot_base_id,
                                  const std::string& payload) {
    std::string body = std::string("{\"agent_id\":\"") + agent + "\","
        "\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"" + payload + "\","
        "\"parent_state_id\":\"" + parent_id + "\","
        "\"snapshot_base_state_id\":\"" + snapshot_base_id + "\","
        "\"sync\":true}";
    std::string r = do_append(env, body);
    REQUIRE(json_ok(r));
    std::string sid = json_str(r, "state_id");
    REQUIRE(is_hex64(sid));
    REQUIRE(json_str(r, "durability") == "durable");
    return sid;
}

// ---------------------------------------------------------------------------
// Sleeper process helpers (mirror test_runtime_control.cpp). The cooperative
// snapshot rendezvous needs a REAL live process so the supervisor's
// process-alive checks pass and the union state gets published.
// ---------------------------------------------------------------------------

static pid_t spawn_sleeper() {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
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

// ---------------------------------------------------------------------------
// Snapshot rendezvous helper. Drives runtime.snapshot + runtime.boundary +
// runtime.template.ready across three threads. Mirrors the equivalent helper
// in test_runtime_control.cpp, but threads the agent_state_id through the
// snapshot request so the linkage test can assert it lands in the union state.
// ---------------------------------------------------------------------------

struct SnapshotOutcome {
    bool ok = false;
    std::string union_state_id;
    std::string fs_commit;
    std::string template_id;
    std::string restore_eligibility;
    std::string raw_snapshot;
};

static SnapshotOutcome do_snapshot_with_agent_state(TestDaemon& env,
                                                    const std::string& runtime_id,
                                                    int64_t boundary_pid,
                                                    pid_t template_pid,
                                                    const std::string& agent_state_id,
                                                    const std::string& token = "tok-rt") {
    Daemon& d = env.daemon;
    SnapshotOutcome out;

    std::string snap_line = "runtime.snapshot {\"runtime_id\":\"" +
        runtime_id + "\",\"boundary_kind\":\"manual\",\"timeout_ms\":5000";
    if (!agent_state_id.empty()) {
        snap_line += ",\"agent_state_id\":\"" + agent_state_id + "\"";
    }
    snap_line += "}";

    std::string snap_response;
    std::thread snap_thread([&] {
        snap_response = control_protocol::dispatch(d, snap_line);
    });

    // Prove the snapshot is parked at wait_for_boundary before the runtime
    // reports the boundary.
    for (int i = 0; i < 50 && snap_response.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(snap_response.empty());  // parked waiting for boundary

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
    (void)ready_response;

    out.raw_snapshot = snap_response;
    out.ok = json_ok(snap_response);
    out.union_state_id = json_str(snap_response, "union_state_id");
    out.fs_commit = json_str(snap_response, "fs_commit");
    out.template_id = template_id;
    out.restore_eligibility = json_str(snap_response, "restore_eligibility");
    return out;
}

// Register a cooperative runtime against a fresh sleeper process group.
static void register_runtime(TestDaemon& env,
                             const std::string& runtime_id,
                             pid_t root_pid,
                             const std::string& token = "tok-rt") {
    std::string r = control_protocol::dispatch(
        env.daemon, "runtime.create {\"runtime_id\":\"" + runtime_id +
        "\",\"branch\":\"main\","
        "\"root_pid\":" + std::to_string((long)root_pid) +
        ",\"process_group_id\":" + std::to_string((long)root_pid) +
        ",\"command_ref\":\"fixture\",\"cooperative\":true,"
        "\"control_token\":\"" + token + "\"}");
    REQUIRE(json_ok(r));
}

// ---------------------------------------------------------------------------
// Test 1: state.append returns a 64-hex state_id.
// ---------------------------------------------------------------------------

static void test_state_append_returns_64_hex_state_id() {
    TestDaemon env("append");
    Daemon& d = env.daemon;

    // Logical-only append: still content-addressed -> 64-hex state_id.
    std::string r = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"hello\",\"sync\":false}");
    REQUIRE(json_ok(r));
    std::string sid = json_str(r, "state_id");
    REQUIRE(is_hex64(sid));
    REQUIRE(json_str(r, "durability") == "logical_only");

    // The state_id is exactly the CAS hash of the serialized record; describe
    // via the protocol (exercised fully in test 2) round-trips it.
    (void)d;

    std::printf("  PASS test_state_append_returns_64_hex_state_id\n");
}

// ---------------------------------------------------------------------------
// Test 2: state.describe returns payload-bearing state JSON.
// ---------------------------------------------------------------------------

static void test_state_describe_returns_payload_bearing_json() {
    TestDaemon env("describe");

    std::string sid = append_logical_root(env, "codex", "session-payload");

    std::string r = control_protocol::dispatch(
        env.daemon, "state.describe {\"state_id\":\"" + sid + "\"}");
    REQUIRE(json_ok(r));
    // Payload-bearing: the inline payload round-trips through describe.
    REQUIRE(json_str(r, "payload_inline") == "session-payload");
    REQUIRE(json_str(r, "state_id") == sid);
    REQUIRE(json_str(r, "agent_id") == "codex");
    REQUIRE(json_str(r, "branch") == "main");
    REQUIRE(json_str(r, "kind") == "session");
    REQUIRE(json_str(r, "payload_schema") == "agentvfs.session.v1");

    std::printf("  PASS test_state_describe_returns_payload_bearing_json\n");
}

// ---------------------------------------------------------------------------
// Test 2b: state.append parses JSON-escaped string payloads losslessly.
// ---------------------------------------------------------------------------

static void test_state_append_round_trips_json_escaped_payload() {
    TestDaemon env("append-escaped-payload");

    const std::string expected = "{\"turn\":1,\"note\":\"quote \\\" ok\"}";
    std::string r = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"{\\\"turn\\\":1,\\\"note\\\":\\\"quote \\\\\\\" ok\\\"}\","
        "\"sync\":false}");
    REQUIRE(json_ok(r));
    std::string sid = json_str(r, "state_id");
    REQUIRE(is_hex64(sid));

    auto d = env.daemon.agent_state().describe(sid);
    REQUIRE(d.ok);
    REQUIRE(d.record.payload_inline == expected);

    const std::string expected_unicode =
        std::string("path / snowman ") + "\xE2""\x98""\x83";
    std::string r2 = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"path \\/ snowman \\u2603\","
        "\"sync\":false}");
    REQUIRE(json_ok(r2));
    std::string sid2 = json_str(r2, "state_id");
    REQUIRE(is_hex64(sid2));

    auto d2 = env.daemon.agent_state().describe(sid2);
    REQUIRE(d2.ok);
    REQUIRE(d2.record.payload_inline == expected_unicode);

    const std::string expected_surrogate =
        std::string("emoji ") + "\xF0""\x9F""\x98""\x80";
    std::string r3 = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"emoji \\uD83D\\uDE00\","
        "\"sync\":false}");
    REQUIRE(json_ok(r3));
    std::string sid3 = json_str(r3, "state_id");
    REQUIRE(is_hex64(sid3));

    auto d3 = env.daemon.agent_state().describe(sid3);
    REQUIRE(d3.ok);
    REQUIRE(d3.record.payload_inline == expected_surrogate);

    std::string malformed_surrogate = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"bad \\uD83D\","
        "\"sync\":false}");
    REQUIRE(!json_ok(malformed_surrogate));
    REQUIRE(malformed_surrogate.find("malformed payload") != std::string::npos);

    std::string malformed = do_append(env,
        "{\"agent_id\":\"codex\",\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"bad\\q\","
        "\"sync\":false}");
    REQUIRE(!json_ok(malformed));
    REQUIRE(malformed.find("malformed payload") != std::string::npos);

    std::printf("  PASS test_state_append_round_trips_json_escaped_payload\n");
}

// ---------------------------------------------------------------------------
// Test 3: state.latest returns the most-recent durable state.
// ---------------------------------------------------------------------------

static void test_state_latest_returns_most_recent_durable() {
    TestDaemon env("latest");

    std::string anchor = append_logical_root(env, "codex", "anchor");
    std::string s1 = append_durable(env, "codex", anchor, anchor, "first-durable");
    std::string s2 = append_durable(env, "codex", s1, anchor, "second-durable");

    std::string r = control_protocol::dispatch(
        env.daemon,
        "state.latest {\"agent_id\":\"codex\",\"branch\":\"main\"}");
    REQUIRE(json_ok(r));
    // The most recently published durable ref wins -> s2.
    REQUIRE(json_str(r, "state_id") == s2);
    REQUIRE(json_str(r, "payload_inline") == "second-durable");

    // Unknown agent+branch pair has no latest ref.
    std::string r2 = control_protocol::dispatch(
        env.daemon,
        "state.latest {\"agent_id\":\"nobody\",\"branch\":\"main\"}");
    REQUIRE(!json_ok(r2));

    std::printf("  PASS test_state_latest_returns_most_recent_durable\n");
}

// ---------------------------------------------------------------------------
// Test 4: state.restore mode=session returns the bounded semantic chain
//         (newest-first, walking parent links back to the snapshot base).
// ---------------------------------------------------------------------------

static void test_state_restore_session_returns_bounded_chain() {
    TestDaemon env("restore-session");

    std::string anchor = append_logical_root(env, "codex", "anchor");
    std::string a = append_durable(env, "codex", anchor, anchor, "step-A");
    std::string b = append_durable(env, "codex", a, anchor, "step-B");

    std::string r = control_protocol::dispatch(
        env.daemon,
        "state.restore {\"state_id\":\"" + b + "\",\"mode\":\"session\"}");
    REQUIRE(json_ok(r));
    REQUIRE(json_str(r, "mode") == "session");
    // Chain is newest-first: [B, A, anchor].
    // Count the chain entries by counting "state_id" occurrences after the
    // first one (the top-level "state" object also has one).
    // Easier: each chain entry carries payload_inline; assert ordering by
    // checking substrings appear newest-first.
    auto posB = r.find("step-B");
    auto posA = r.find("step-A");
    auto posAnchor = r.find("anchor");
    REQUIRE(posB != std::string::npos);
    REQUIRE(posA != std::string::npos);
    REQUIRE(posAnchor != std::string::npos);
    REQUIRE(posB < posA);
    REQUIRE(posA < posAnchor);
    // The requested state's id is the first chain entry.
    REQUIRE(r.find(b) != std::string::npos);

    std::printf("  PASS test_state_restore_session_returns_bounded_chain\n");
}

// ---------------------------------------------------------------------------
// Test 5: state.restore mode=full rejects a state without an fs_commit.
// ---------------------------------------------------------------------------

static void test_state_restore_full_rejects_state_without_fs_commit() {
    TestDaemon env("restore-full");

    // A session-only state has no fs_commit (defaults to ZERO_HASH).
    std::string sid = append_logical_root(env, "codex", "no-fs-commit");

    std::string r = control_protocol::dispatch(
        env.daemon,
        "state.restore {\"state_id\":\"" + sid + "\",\"mode\":\"full\"}");
    REQUIRE(!json_ok(r));
    // The rejection must mention fs_commit so the operator can diagnose it.
    REQUIRE(r.find("fs_commit") != std::string::npos);

    std::printf("  PASS test_state_restore_full_rejects_state_without_fs_commit\n");
}

// ---------------------------------------------------------------------------
// Test 6: runtime.snapshot rejects a malformed direct agent_state_id BEFORE
//         snapshot orchestration starts.
// ---------------------------------------------------------------------------

static void test_runtime_snapshot_rejects_malformed_agent_state_id() {
    TestDaemon env("snap-validate");

    // Provide a (fake) runtime_id so the runtime_id emptiness guard does not
    // fire first; the agent_state_id validation must reject the request before
    // Daemon::snapshot_runtime() is ever called.
    std::string r = control_protocol::dispatch(
        env.daemon,
        "runtime.snapshot {\"runtime_id\":\"rt-fake\","
        "\"agent_state_id\":\"not-hex\","
        "\"boundary_kind\":\"manual\",\"timeout_ms\":50}");
    REQUIRE(!json_ok(r));
    REQUIRE(r.find("invalid agent_state_id") != std::string::npos);

    // Overlong but otherwise hex-looking values must be rejected, not
    // truncated to their first 64 chars by hex_to_hash().
    std::string overlong_hex(65, 'a');
    std::string r_overlong = control_protocol::dispatch(
        env.daemon,
        "runtime.snapshot {\"runtime_id\":\"rt-fake\","
        "\"agent_state_id\":\"" + overlong_hex + "\","
        "\"boundary_kind\":\"manual\",\"timeout_ms\":50}");
    REQUIRE(!json_ok(r_overlong));
    REQUIRE(r_overlong.find("invalid agent_state_id") != std::string::npos);

    std::string r_malformed = control_protocol::dispatch(
        env.daemon,
        "runtime.snapshot {\"runtime_id\":\"rt-fake\","
        "\"agent_state_id\":\"bad\\q\","
        "\"boundary_kind\":\"manual\",\"timeout_ms\":50}");
    REQUIRE(!json_ok(r_malformed));
    REQUIRE(r_malformed.find("invalid agent_state_id") != std::string::npos);

    // A well-formed 64-hex agent_state_id passes the format guard (it will
    // then fail downstream because no real runtime is registered, but the
    // guard itself must accept it).
    std::string hex64(64, '0');
    std::string r2 = control_protocol::dispatch(
        env.daemon,
        "runtime.snapshot {\"runtime_id\":\"rt-fake\","
        "\"agent_state_id\":\"" + hex64 + "\","
        "\"boundary_kind\":\"manual\",\"timeout_ms\":50}");
    REQUIRE(r2.find("invalid agent_state_id") == std::string::npos);

    // Empty agent_state_id is explicitly allowed by the guard.
    std::string r3 = control_protocol::dispatch(
        env.daemon,
        "runtime.snapshot {\"runtime_id\":\"rt-fake2\","
        "\"boundary_kind\":\"manual\",\"timeout_ms\":50}");
    REQUIRE(r3.find("invalid agent_state_id") == std::string::npos);

    std::printf("  PASS test_runtime_snapshot_rejects_malformed_agent_state_id\n");
}

// ---------------------------------------------------------------------------
// Test 6b: runtime.restore rejects overlong union_state_id before restore.
// ---------------------------------------------------------------------------

static void test_runtime_restore_rejects_overlong_union_state_id() {
    TestDaemon env("restore-overlong");

    std::string overlong_hex(65, 'a');
    std::string r = control_protocol::dispatch(
        env.daemon,
        "runtime.restore {\"union_state_id\":\"" + overlong_hex +
        "\",\"timeout_ms\":50}");
    REQUIRE(!json_ok(r));
    REQUIRE(r.find("invalid union_state_id") != std::string::npos);

    std::string malformed = control_protocol::dispatch(
        env.daemon,
        "runtime.restore {\"union_state_id\":\"bad\\q\","
        "\"timeout_ms\":50}");
    REQUIRE(!json_ok(malformed));
    REQUIRE(malformed.find("invalid union_state_id") != std::string::npos);

    std::printf("  PASS test_runtime_restore_rejects_overlong_union_state_id\n");
}

// ---------------------------------------------------------------------------
// Test 7: end-to-end linkage — semantic state -> runtime.snapshot ->
//         UnionRuntimeState.agent_state_id -> runtime_snapshot state record.
//
// This is the core Task-4 invariant: a controller appends a semantic state,
// hands its id to runtime.snapshot via agent_state_id, and the resulting
// union state MUST store that same id; the returned union_state_id MUST then
// round-trip through state.append (kind=runtime_snapshot) and state.describe.
// ---------------------------------------------------------------------------

static void test_runtime_snapshot_links_agent_state_to_union_state() {
    TestDaemon env("snap-link");
    Daemon& d = env.daemon;

    // 1. Append a semantic session state -> state_id.
    std::string semantic_id = append_logical_root(env, "codex", "pre-snapshot");

    // 2. Register a cooperative runtime against a real sleeper process group.
    pid_t root_pid = spawn_sleeper();
    pid_t template_pid = spawn_sleeper();
    register_runtime(env, "rt-link", root_pid);

    // Seed the branch with real content so the snapshot has a non-empty
    // fs_commit (mirrors test_runtime_control.cpp).
    auto main = d.main_branch();
    Hash blob = d.store().write_blob(
        reinterpret_cast<const uint8_t*>("link"), 4);
    REQUIRE(blob != ZERO_HASH);
    main->wt.insert("/linked.txt", {EntryKind::Blob, blob, 0100644});

    // 3. Drive the snapshot rendezvous WITH the semantic agent_state_id.
    SnapshotOutcome snap = do_snapshot_with_agent_state(
        env, "rt-link", root_pid, template_pid, semantic_id);
    REQUIRE(snap.ok);
    REQUIRE(is_hex64(snap.union_state_id));
    REQUIRE(is_hex64(snap.fs_commit));

    // 4. The published UnionRuntimeState MUST carry the SAME agent_state_id.
    UnionRuntimeState us;
    std::string err;
    Hash union_hash;
    REQUIRE(hex_to_hash(snap.union_state_id.c_str(), union_hash));
    REQUIRE(read_union_runtime_state(d.store(), union_hash, us, err));
    REQUIRE(us.agent_state_id == semantic_id);

    // 5. A runtime_snapshot state record can store the returned union_state_id
    //    and round-trip it back through state.describe.
    std::string body = std::string("{\"agent_id\":\"codex\","
        "\"branch\":\"main\","
        "\"kind\":\"runtime_snapshot\","
        "\"payload_schema\":\"agentvfs.runtime_snapshot.v1\","
        "\"payload\":\"snapshot-of-rt-link\","
        "\"runtime_id\":\"rt-link\","
        "\"union_state_id\":\"") + snap.union_state_id + "\","
        "\"sync\":false}";
    std::string r = do_append(env, body);
    REQUIRE(json_ok(r));
    std::string snap_state_id = json_str(r, "state_id");
    REQUIRE(is_hex64(snap_state_id));

    // describe round-trips kind=runtime_snapshot + union_state_id verbatim.
    std::string desc = control_protocol::dispatch(
        env.daemon, "state.describe {\"state_id\":\"" + snap_state_id + "\"}");
    REQUIRE(json_ok(desc));
    REQUIRE(json_str(desc, "state_id") == snap_state_id);
    REQUIRE(json_str(desc, "kind") == "runtime_snapshot");
    REQUIRE(json_str(desc, "union_state_id") == snap.union_state_id);
    REQUIRE(json_str(desc, "runtime_id") == "rt-link");

    kill_and_reap(template_pid);
    kill_and_reap(root_pid);

    std::printf("  PASS test_runtime_snapshot_links_agent_state_to_union_state\n");
}

// ---------------------------------------------------------------------------
// Test 8: rollback against a missing branch returns "unknown branch" (the
// original, pre-refactor contract). The Task-3 refactor moved target
// resolution ahead of the branch lookup, which masked this case as
// "target commit not found". This test pins the restored ordering.
// ---------------------------------------------------------------------------

static void test_rollback_missing_branch_returns_unknown_branch() {
    TestDaemon env("rollback-missing");

    // A label target against a branch that does not exist must surface as
    // "unknown branch", not "target commit not found".
    std::string r = control_protocol::dispatch(
        env.daemon, "rollback cp1 branch=does-not-exist");
    REQUIRE(!json_ok(r));
    REQUIRE(r.find("unknown branch") != std::string::npos);
    REQUIRE(r.find("target commit not found") == std::string::npos);

    // Same contract for a hex target against a missing branch.
    std::string hex64(64, '0');
    std::string r2 = control_protocol::dispatch(
        env.daemon, "rollback " + hex64 + " branch=still-missing");
    REQUIRE(!json_ok(r2));
    REQUIRE(r2.find("unknown branch") != std::string::npos);

    std::printf("  PASS test_rollback_missing_branch_returns_unknown_branch\n");
}

// ---------------------------------------------------------------------------
// Test 9: state.append partial-success surfaces the orphan state_id. When the
// state blob is made durable (fsync'd) but the latest-ref publish fails, the
// service returns ok=false WITH state_id populated; the control handler must
// surface state_id + durability so a controller can record the orphan.
// ---------------------------------------------------------------------------

static void test_state_append_partial_success_surfaces_orphan_state_id() {
    TestDaemon env("append-partial");

    // Anchor must exist (logical-only) for the sync=true validation guard.
    std::string anchor_id = append_logical_root(env, "codex", "anchor");

    // Sabotage the latest-ref destination for agent "codex" so that
    // publish_latest_ref fails AFTER write_agent_state_record has already
    // durably committed (fsync'd) the state blob. Creating
    // <store>/state/latest/codex as a regular FILE makes mkstemp inside it
    // fail with ENOTDIR, deterministically and regardless of the test user's
    // privileges (even root cannot mkstemp inside a non-directory).
    std::string state_dir = env.store_root + "/state";
    std::string latest_dir = state_dir + "/latest";
    REQUIRE(::mkdir(state_dir.c_str(), 0777) == 0 || errno == EEXIST);
    REQUIRE(::mkdir(latest_dir.c_str(), 0777) == 0 || errno == EEXIST);
    std::string agent_path = latest_dir + "/codex";
    FILE* sabotage = std::fopen(agent_path.c_str(), "w");
    REQUIRE(sabotage != nullptr);
    std::fclose(sabotage);

    // Durable append: blob write succeeds, ref publish fails -> partial success.
    std::string body = std::string("{\"agent_id\":\"codex\",")
        + "\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"partial-orphan\","
        "\"parent_state_id\":\"" + anchor_id + "\","
        "\"snapshot_base_state_id\":\"" + anchor_id + "\","
        "\"sync\":true}";
    std::string r = do_append(env, body);
    REQUIRE(!json_ok(r));
    // The orphan state_id MUST appear so a controller can record it; the
    // durability says exactly how durable the orphan is.
    std::string orphan = json_str(r, "state_id");
    REQUIRE(is_hex64(orphan));
    REQUIRE(json_str(r, "durability") == "durable");
    // The error string still explains what failed.
    REQUIRE(r.find("latest ref") != std::string::npos);

    // The orphan state IS readable via state.describe: the content-addressed
    // hash survived the ref-publish failure and the blob is durable.
    std::string desc = control_protocol::dispatch(
        env.daemon, "state.describe {\"state_id\":\"" + orphan + "\"}");
    REQUIRE(json_ok(desc));
    REQUIRE(json_str(desc, "payload_inline") == "partial-orphan");
    REQUIRE(json_str(desc, "state_id") == orphan);

    std::printf("  PASS test_state_append_partial_success_surfaces_orphan_state_id\n");
}

// ---------------------------------------------------------------------------
// Test 10: state.restore mode=full SUCCEEDS when fs_commit references a REAL
// commit in the store, rolling the branch back to that commit and surfacing
// the semantic record. (mode=runtime is exercised end-to-end by the linkage
// test in test_runtime_control.cpp; it needs a cooperative runtime and is
// intentionally not duplicated here.)
// ---------------------------------------------------------------------------

static void test_state_restore_full_succeeds_with_real_fs_commit() {
    TestDaemon env("restore-full-ok");
    Daemon& d = env.daemon;

    // Seed the working tree with real content and checkpoint it so the branch
    // has a REAL commit the restore can anchor on.
    auto main = d.main_branch();
    Hash blob = d.store().write_blob(
        reinterpret_cast<const uint8_t*>("full-restore"), 12);
    REQUIRE(blob != ZERO_HASH);
    main->wt.insert("/full_restore.txt", {EntryKind::Blob, blob, 0100644});

    std::string cp = control_protocol::dispatch(env.daemon, "checkpoint cp-full");
    REQUIRE(json_ok(cp));
    std::string commit_hex = json_str(cp, "commit");
    REQUIRE(is_hex64(commit_hex));

    // Append a durable state whose fs_commit references the real commit.
    std::string anchor = append_logical_root(env, "codex", "full-anchor");
    std::string body = std::string("{\"agent_id\":\"codex\",")
        + "\"branch\":\"main\","
        "\"kind\":\"session\","
        "\"payload_schema\":\"agentvfs.session.v1\","
        "\"payload\":\"full-restore-state\","
        "\"parent_state_id\":\"" + anchor + "\","
        "\"snapshot_base_state_id\":\"" + anchor + "\","
        "\"fs_commit\":\"" + commit_hex + "\","
        "\"sync\":true}";
    std::string ar = do_append(env, body);
    REQUIRE(json_ok(ar));
    std::string sid = json_str(ar, "state_id");
    REQUIRE(is_hex64(sid));

    // state.restore mode=full rolls the branch back to fs_commit and surfaces
    // the semantic record.
    std::string r = control_protocol::dispatch(
        env.daemon,
        "state.restore {\"state_id\":\"" + sid + "\",\"mode\":\"full\"}");
    REQUIRE(json_ok(r));
    REQUIRE(json_str(r, "mode") == "full");
    REQUIRE(json_str(r, "rolled_back_to") == commit_hex);
    // The semantic record is surfaced verbatim.
    REQUIRE(json_str(r, "payload_inline") == "full-restore-state");
    REQUIRE(json_str(r, "fs_commit") == commit_hex);

    std::printf("  PASS test_state_restore_full_succeeds_with_real_fs_commit\n");
}

int main() {
    std::printf("test_agent_state_control:\n");
    test_state_append_returns_64_hex_state_id();
    test_state_describe_returns_payload_bearing_json();
    test_state_append_round_trips_json_escaped_payload();
    test_state_latest_returns_most_recent_durable();
    test_state_restore_session_returns_bounded_chain();
    test_state_restore_full_rejects_state_without_fs_commit();
    test_runtime_snapshot_rejects_malformed_agent_state_id();
    test_runtime_restore_rejects_overlong_union_state_id();
    test_runtime_snapshot_links_agent_state_to_union_state();
    test_rollback_missing_branch_returns_unknown_branch();
    test_state_append_partial_success_surfaces_orphan_state_id();
    test_state_restore_full_succeeds_with_real_fs_commit();
    std::printf("PASS test_agent_state_control\n");
    return 0;
}
