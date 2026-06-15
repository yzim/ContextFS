#include "backends/bpftime_backend.h"

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

static void test_name_and_capabilities() {
    cas::BpftimeBackend backend;
    cas::Capabilities caps = backend.capabilities();

    CHECK(backend.name() == "bpftime");
    // The degraded stub cannot execute probes, so it must not advertise
    // any op coverage until a compatible bpftime runtime API is integrated.
    CHECK(caps.supported_ops == 0);
    CHECK(!caps.pre_op_verdicts);
    CHECK(!caps.requires_cgroup);
    CHECK(!caps.requires_root);
}

static void test_start_without_runtime_fails_without_callbacks() {
    cas::BpftimeBackend backend;
    bool callback_called = false;

    CHECK(!backend.start(cas::BackendConfig{}, [&](cas::TelemetryEvent) {
        callback_called = true;
    }));
    backend.stop();
    backend.stop();
    CHECK(!callback_called);
}

static void test_cold_stop_is_idempotent() {
    cas::BpftimeBackend backend;
    backend.stop();
    backend.stop();
}

static void test_session_and_policy_hooks_are_safe_stubs() {
    cas::BpftimeBackend backend;
    cas::SessionInfo session{};
    session.cgroup_path = "/sys/fs/cgroup/agentvfs-test";
    cas::PolicyRules rules{};

    CHECK(backend.register_session(session));
    CHECK(backend.unregister_session(session.cgroup_path));
    CHECK(backend.install_policy(rules));
}

int main() {
    test_name_and_capabilities();
    test_start_without_runtime_fails_without_callbacks();
    test_cold_stop_is_idempotent();
    test_session_and_policy_hooks_are_safe_stubs();
    std::fprintf(stderr, "test_bpftime_backend: all tests passed\n");
    return 0;
}
