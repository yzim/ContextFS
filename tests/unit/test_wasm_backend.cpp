#include "backends/wasm_backend.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expr);                                   \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

static bool same_event(const cas::TelemetryEvent& left,
                       const cas::TelemetryEvent& right) {
    return left.timestamp_ns == right.timestamp_ns &&
           left.session_id == right.session_id &&
           left.branch_id == right.branch_id &&
           left.policy_version == right.policy_version &&
           left.op == right.op &&
           left.verdict == right.verdict &&
           left.dev == right.dev &&
           left.ino == right.ino &&
           left.i_generation == right.i_generation &&
           left.path == right.path &&
           left.pid == right.pid &&
           left.uid == right.uid &&
           left.gid == right.gid &&
           left.bytes == right.bytes &&
           left.latency_ns == right.latency_ns &&
           left.backend == right.backend &&
           left.extra == right.extra;
}

static cas::TelemetryEvent sample_event() {
    cas::TelemetryEvent ev{};
    ev.timestamp_ns = 123456789;
    ev.session_id = 42;
    ev.branch_id = 7;
    ev.policy_version = 3;
    ev.op = cas::OpType::Write;
    ev.verdict = cas::Verdict::SoftWatch;
    ev.dev = 8;
    ev.ino = 99;
    ev.i_generation = 2;
    ev.path = "/tmp/agentvfs-wasm.txt";
    ev.pid = 1234;
    ev.uid = 1000;
    ev.gid = 1000;
    ev.bytes = 4096;
    ev.latency_ns = 500;
    ev.backend = "ebpf";
    ev.extra.push_back({"key", "value"});
    return ev;
}

static void test_degraded_stub_name_and_capabilities() {
    cas::WasmBackend backend;
    cas::Capabilities caps = backend.capabilities();

    CHECK(backend.name() == "wasm");
    CHECK(caps.supported_ops == 0);
    // The Task 9 degraded stub cannot execute modules, so it must not
    // advertise verdict support until real WAMR integration exists.
    CHECK(!caps.pre_op_verdicts);
    CHECK(!caps.requires_cgroup);
    CHECK(!caps.requires_root);
}

static void test_start_without_module_fails_without_callbacks() {
    cas::WasmBackend backend;
    bool callback_called = false;

    CHECK(!backend.start(cas::BackendConfig{}, [&](cas::TelemetryEvent) {
        callback_called = true;
    }));
    CHECK(!callback_called);
    backend.stop();
}

static void test_start_with_module_fails_without_runtime_callbacks() {
    cas::WasmBackend backend;
    cas::BackendConfig cfg{};
    bool callback_called = false;

    cfg.params["module_path"] = "/tmp/agentvfs-test-filter.wasm";
    CHECK(!backend.start(cfg, [&](cas::TelemetryEvent) {
        callback_called = true;
    }));
    CHECK(!callback_called);
    backend.stop();
}

static void test_stop_is_idempotent() {
    cas::WasmBackend backend;
    backend.stop();
    backend.stop();
}

static void test_process_event_passes_through_when_not_started() {
    cas::WasmBackend backend;
    cas::TelemetryEvent ev = sample_event();
    cas::TelemetryEvent processed = backend.process_event(ev);

    CHECK(same_event(processed, ev));
}

static void test_session_and_policy_hooks_are_safe_stubs() {
    cas::WasmBackend backend;
    cas::SessionInfo session{};
    cas::PolicyRules rules{};

    session.cgroup_path = "/sys/fs/cgroup/agentvfs-wasm-test";
    CHECK(backend.register_session(session));
    CHECK(backend.unregister_session(session.cgroup_path));
    CHECK(backend.install_policy(rules));
}

int main() {
    test_degraded_stub_name_and_capabilities();
    test_start_without_module_fails_without_callbacks();
    test_start_with_module_fails_without_runtime_callbacks();
    test_stop_is_idempotent();
    test_process_event_passes_through_when_not_started();
    test_session_and_policy_hooks_are_safe_stubs();
    std::fprintf(stderr, "test_wasm_backend: all tests passed\n");
    return 0;
}
