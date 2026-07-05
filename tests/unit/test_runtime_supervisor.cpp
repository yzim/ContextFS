#include "runtime_supervisor.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

class FakeProcessController : public RuntimeProcessController {
public:
    bool alive = true;
    std::vector<int64_t> frozen;
    std::vector<int64_t> resumed;
    std::vector<int64_t> terminated;

    bool process_alive(int64_t pid) override {
        return alive && pid > 0;
    }

    bool freeze_process_group(int64_t pgid, std::string& error) override {
        frozen.push_back(pgid);
        error.clear();
        return pgid > 0;
    }

    bool resume_process_group(int64_t pgid, std::string& error) override {
        resumed.push_back(pgid);
        error.clear();
        return pgid > 0;
    }

    bool terminate_process_group(int64_t pgid, std::string& error) override {
        terminated.push_back(pgid);
        error.clear();
        return pgid > 0;
    }
};

static RuntimeCreateRequest create_request() {
    RuntimeCreateRequest req;
    req.runtime_id = "rt-1";
    req.branch = "main";
    req.root_pid = 1200;
    req.process_group_id = 1200;
    req.command_ref = "argv:counter";
    req.cwd = "/tmp";
    req.cooperative = true;
    req.control_token = "tok-1";
    return req;
}

static void test_register_and_status() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));
    REQUIRE(error.empty());

    RuntimeStatus st;
    REQUIRE(sup.status("rt-1", st, error));
    REQUIRE(st.runtime_id == "rt-1");
    REQUIRE(st.branch == "main");
    REQUIRE(st.root_pid == 1200);
    REQUIRE(st.generation == 1);
    REQUIRE(st.restore_eligibility == RestoreEligibility::MetadataOnly);
    REQUIRE(st.cooperative);
}

static void test_pending_snapshot_boundary_and_template_ready() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "before_tool";
    req.agent_state_id = std::string(64, 'a');
    req.timeout_ms = 10;
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    REQUIRE(!op_id.empty());

    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "before_tool", boundary_id, error));
    REQUIRE(!boundary_id.empty());
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(action.action == "snapshot");
    REQUIRE(action.operation_id == op_id);
    REQUIRE(!action.template_id.empty());

    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id, 1300, 1300, 1, error));
    TemplateStatus tmpl;
    REQUIRE(sup.template_status(action.template_id, tmpl, error));
    REQUIRE(tmpl.template_pid == 1300);
    REQUIRE(tmpl.template_process_group_id == 1300);
    REQUIRE(tmpl.alive);
}

static void test_template_loss_degrades_status() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));
    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id, 1300, 1300, 1, error));

    pc.alive = false;
    TemplateStatus tmpl;
    REQUIRE(sup.template_status(action.template_id, tmpl, error));
    REQUIRE(!tmpl.alive);

    RuntimeStatus st;
    REQUIRE(sup.status("rt-1", st, error));
    REQUIRE(st.restore_eligibility == RestoreEligibility::FsOnly);
}

static void test_restore_terminates_active_generation_and_waits_for_template() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));
    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id, 1300, 1300, 1, error));

    RuntimeRestoreIntent intent;
    intent.runtime_id = "rt-1";
    intent.template_id = action.template_id;
    intent.target_generation = 2;
    REQUIRE(sup.begin_restore(intent, error));
    REQUIRE(pc.frozen.size() == 1);
    REQUIRE(pc.frozen[0] == 1200);
    REQUIRE(pc.terminated.empty());

    RuntimeTemplateAction template_action;
    REQUIRE(sup.template_poll(action.template_id, "tok-1", template_action, error));
    REQUIRE(template_action.action == "wait");
    REQUIRE(template_action.target_generation == 0);
    REQUIRE(sup.publish_restore("rt-1", error));
    REQUIRE(sup.template_poll(action.template_id, "tok-1", template_action, error));
    REQUIRE(template_action.action == "restore");
    REQUIRE(template_action.target_generation == 2);

    REQUIRE(sup.generation_ready("rt-1", "tok-1", 2200, 2200, 2, error));
    // generation_ready no longer terminates the prior group itself: it stashes
    // the prior pgid (here 1200) for the restore requester to retire, so the
    // restored runtime's ready acknowledgement is not blocked on the kill.
    REQUIRE(pc.terminated.empty());
    REQUIRE(sup.retire_pending_generation("rt-1", error));
    REQUIRE(pc.terminated.size() == 1);
    REQUIRE(pc.terminated[0] == 1200);
    RuntimeStatus st;
    REQUIRE(sup.status("rt-1", st, error));
    REQUIRE(st.root_pid == 2200);
    REQUIRE(st.generation == 2);
    REQUIRE(st.active_process_group_id == 2200);
}

static void test_lifecycle_rejects_invalid_control_tokens() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));

    std::string boundary_id;
    REQUIRE(!sup.observe_boundary("rt-1", "wrong", 1200, 1, "manual",
                                  boundary_id, error));
    REQUIRE(error == "invalid control token");
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual",
                                 boundary_id, error));

    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(!sup.template_ready("rt-1", "wrong", action.template_id,
                                1300, 1300, 1, error));
    REQUIRE(error == "invalid control token");
    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id,
                               1300, 1300, 1, error));

    RuntimeTemplateAction poll;
    REQUIRE(!sup.template_poll(action.template_id, "wrong", poll, error));
    REQUIRE(error == "invalid control token");

    RuntimeRestoreIntent intent;
    intent.runtime_id = "rt-1";
    intent.template_id = action.template_id;
    intent.target_generation = 2;
    REQUIRE(sup.begin_restore(intent, error));
    REQUIRE(sup.publish_restore("rt-1", error));
    REQUIRE(sup.template_poll(action.template_id, "tok-1", poll, error));
    REQUIRE(poll.action == "restore");
    REQUIRE(!sup.generation_ready("rt-1", "wrong", 2200, 2200, 2, error));
    REQUIRE(error == "invalid control token");
}

// --- Two-thread tests for the blocking coordination primitives ----------

// Test 1: boundary rendezvous across the daemon thread (wait_for_boundary +
// release_boundary_for_snapshot) and a runtime handler thread (observe_boundary
// + wait_boundary_action). Both sides must meet with action=="snapshot".
static void test_boundary_rendezvous_two_threads() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "before_tool";
    req.agent_state_id = std::string(64, 'a');
    req.timeout_ms = 1000;
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    REQUIRE(!op_id.empty());

    const std::string captured_op_id = op_id;
    std::string captured_template_id;
    std::string runtime_seen_boundary_id;

    std::thread daemon([&]() {
        std::string err;
        std::string boundary_id;
        // Block until the runtime observes the boundary.
        bool ok = sup.wait_for_boundary("rt-1", 1000, boundary_id, err);
        REQUIRE(ok);
        REQUIRE(err.empty());
        REQUIRE(!boundary_id.empty());
        // Release the boundary for snapshot -> wakes the runtime handler.
        RuntimeBoundaryAction action;
        REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, err));
        REQUIRE(action.action == "snapshot");
        REQUIRE(action.operation_id == captured_op_id);
        REQUIRE(!action.template_id.empty());
        captured_template_id = action.template_id;
    });

    std::thread runtime([&]() {
        std::string err;
        // Let the daemon enter wait_for_boundary first.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::string boundary_id;
        REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "before_tool",
                                     boundary_id, err));
        REQUIRE(!boundary_id.empty());
        runtime_seen_boundary_id = boundary_id;
        // Block until the daemon releases the boundary.
        RuntimeBoundaryAction action;
        bool ok = sup.wait_boundary_action(boundary_id, 1000, action, err);
        REQUIRE(ok);
        REQUIRE(err.empty());
        REQUIRE(action.action == "snapshot");
        REQUIRE(action.operation_id == captured_op_id);
        REQUIRE(!action.template_id.empty());
    });

    daemon.join();
    runtime.join();
    REQUIRE(!captured_template_id.empty());
    REQUIRE(!runtime_seen_boundary_id.empty());
}

// Test 2: timeouts. With no releaser, wait_boundary_action must return false
// with "snapshot timeout"; with no generation_ready, wait_for_generation_ready
// must return false with "restore timeout". Neither call may block the test.
static void test_blocking_timeouts() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));

    RuntimeBoundaryAction action;
    bool ok = sup.wait_boundary_action(boundary_id, 50, action, error);
    REQUIRE(!ok);
    REQUIRE(error == "snapshot timeout");

    std::string err2;
    bool ok2 = sup.wait_for_generation_ready("rt-1", 50, err2);
    REQUIRE(!ok2);
    REQUIRE(err2 == "restore timeout");
}

// Test 3: template-ready rendezvous. Daemon blocks in wait_for_template_ready
// until the runtime calls template_ready; the runtime then blocks in
// wait_template_published until the daemon attaches the union state.
static void test_template_ready_rendezvous_two_threads() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    const std::string template_id = action.template_id;
    REQUIRE(!template_id.empty());

    const std::string union_id = std::string(64, 'c');

    std::thread daemon([&]() {
        std::string err;
        // Block until the runtime reports template_ready.
        bool ok = sup.wait_for_template_ready("rt-1", 1000, err);
        REQUIRE(ok);
        REQUIRE(err.empty());
        // Attach union state -> wakes the runtime's wait_template_published.
        Hash fs_commit{};
        fs_commit[0] = 0x42;
        REQUIRE(sup.attach_union_state(template_id, union_id, fs_commit, err));
    });

    std::thread runtime([&]() {
        std::string err;
        // Let the daemon enter wait_for_template_ready first.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        REQUIRE(sup.template_ready("rt-1", "tok-1", template_id, 1300, 1300, 1, err));
        // Block until the daemon attaches the union state.
        bool ok = sup.wait_template_published(template_id, 1000, err);
        REQUIRE(ok);
        REQUIRE(err.empty());
    });

    daemon.join();
    runtime.join();

    TemplateStatus tmpl;
    REQUIRE(sup.template_status(template_id, tmpl, error));
    REQUIRE(tmpl.union_state_id == union_id);
    REQUIRE(tmpl.alive);
}

// Test 4: fail_template_publish releases a blocked wait_template_published
// waiter with the stored error (NOT "snapshot timeout") and does NOT erase the
// template, so it remains queryable.
static void test_fail_template_publish_releases_blocked_waiter() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    // Drive through begin_snapshot -> observe_boundary -> release_boundary_for_
    // snapshot to materialise a template id.
    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    const std::string template_id = action.template_id;
    REQUIRE(!template_id.empty());

    // Thread T: report template_ready, then block in wait_template_published.
    std::string waiter_error = "untouched";
    bool waiter_ok = true;
    std::thread t([&]() {
        std::string err;
        REQUIRE(sup.template_ready("rt-1", "tok-1", template_id, 1300, 1300, 1, err));
        bool ok = sup.wait_template_published(template_id, 5000, err);
        waiter_ok = ok;
        waiter_error = err;
    });

    // Give T time to enter wait_template_published.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string fail_err;
    REQUIRE(sup.fail_template_publish(template_id, "store full", fail_err));
    REQUIRE(fail_err.empty());

    t.join();
    // The waiter must have been released with the stored error, not parked for
    // the full timeout returning "snapshot timeout".
    REQUIRE(!waiter_ok);
    REQUIRE(waiter_error == "store full");

    // The template must still be queryable (fail_template_publish must NOT
    // erase it, unlike drop_template).
    TemplateStatus tmpl;
    REQUIRE(sup.template_status(template_id, tmpl, error));
    REQUIRE(tmpl.union_state_id.empty());  // never published
}

// Test 5: cancel_snapshot clears a pending snapshot so begin_snapshot can be
// accepted again. Previously a timed-out wait_for_boundary left
// has_pending_snapshot set, permanently breaking snapshots for that runtime.
static void test_cancel_snapshot_restores_snapshot_ability() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    REQUIRE(!op_id.empty());

    // Before cancel, a second begin_snapshot is rejected: a snapshot is in
    // progress.
    std::string op_id2;
    REQUIRE(!sup.begin_snapshot(req, op_id2, error));
    REQUIRE(error == "snapshot already in progress");

    std::string cancel_err;
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));
    REQUIRE(cancel_err.empty());

    // After cancel, begin_snapshot must succeed again with a fresh op id.
    error.clear();
    REQUIRE(sup.begin_snapshot(req, op_id2, error));
    REQUIRE(op_id2 != op_id);

    // cancel_snapshot on an unknown runtime must fail cleanly.
    std::string unknown_err;
    REQUIRE(!sup.cancel_snapshot("rt-missing", unknown_err));
    REQUIRE(unknown_err == "unknown runtime");

    // Clean up the second pending snapshot.
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));
}

// --- C1: late generation_ready after the restore was aborted must be rejected,
// and must NOT overwrite active_process_group_id / generation / root_pid or
// stash a retire_pgid that would never be consumed (single-active invariant).
static void test_late_generation_ready_after_abort_is_rejected() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    // Materialize a template via a snapshot.
    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id, 1300, 1300, 1, error));

    // Start a restore targeting generation 2, then simulate the daemon's
    // wait_for_generation_ready timing out and aborting the restore.
    RuntimeRestoreIntent intent;
    intent.runtime_id = "rt-1";
    intent.template_id = action.template_id;
    intent.target_generation = 2;
    REQUIRE(sup.begin_restore(intent, error));
    REQUIRE(sup.abort_restore("rt-1", error));  // SIGCONTs the frozen 1200

    // Capture the active state BEFORE the late ack.
    RuntimeStatus before;
    REQUIRE(sup.status("rt-1", before, error));
    REQUIRE(before.active_process_group_id == 1200);
    REQUIRE(before.generation == 1);
    REQUIRE(before.root_pid == 1200);

    // The orphaned grandchild calls generation_ready LATE (after the abort).
    std::string late_err;
    bool ok = sup.generation_ready("rt-1", "tok-1", 2200, 2200, 2, late_err);
    REQUIRE(!ok);
    REQUIRE(late_err == "restore aborted or stale");

    // State MUST be unchanged: the late ack did not stash retire_pgid, did not
    // overwrite root_pid / active_process_group_id / generation.
    RuntimeStatus after;
    REQUIRE(sup.status("rt-1", after, error));
    REQUIRE(after.active_process_group_id == 1200);
    REQUIRE(after.generation == 1);
    REQUIRE(after.root_pid == 1200);

    // retire_pending_generation has nothing to terminate (no stash happened).
    std::string retire_err;
    REQUIRE(sup.retire_pending_generation("rt-1", retire_err));
    REQUIRE(pc.terminated.empty());
}

// --- C1 complement: a generation_ready for a STALE generation (a newer restore
// retargeted the runtime) is also rejected.
static void test_stale_generation_ready_for_wrong_generation_is_rejected() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    // Two templates from two snapshots.
    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction a1;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, a1, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", a1.template_id, 1300, 1300, 1, error));
    std::string cancel_err;
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));

    // A restore targeting generation 5.
    RuntimeRestoreIntent intent;
    intent.runtime_id = "rt-1";
    intent.template_id = a1.template_id;
    intent.target_generation = 5;
    REQUIRE(sup.begin_restore(intent, error));

    // A late grandchild reporting generation 2 (stale) must be rejected.
    std::string late_err;
    bool ok = sup.generation_ready("rt-1", "tok-1", 2200, 2200, 2, late_err);
    REQUIRE(!ok);
    REQUIRE(late_err == "restore aborted or stale");

    // The correct generation IS accepted.
    std::string good_err;
    REQUIRE(sup.publish_restore("rt-1", good_err));
    RuntimeTemplateAction action_out;
    REQUIRE(sup.template_poll(a1.template_id, "tok-1", action_out, good_err));
    REQUIRE(action_out.action == "restore");
    REQUIRE(sup.generation_ready("rt-1", "tok-1", 2500, 2500, 5, good_err));
    REQUIRE(good_err.empty());
}

// --- I1: after drop_template, template_poll yields action:"drop" (the parked
// template's exit signal), not {"ok":false,"error":"unknown template"} which
// would spin the client forever.
static void test_drop_template_makes_poll_return_drop() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    const std::string template_id = action.template_id;
    REQUIRE(sup.template_ready("rt-1", "tok-1", template_id, 1300, 1300, 1, error));

    // Before drop, poll returns "wait".
    RuntimeTemplateAction pa;
    REQUIRE(sup.template_poll(template_id, "tok-1", pa, error));
    REQUIRE(pa.action == "wait");

    // After drop, the next poll MUST observe "drop".
    REQUIRE(sup.drop_template(template_id, error));

    RuntimeTemplateAction da;
    REQUIRE(sup.template_poll(template_id, "tok-1", da, error));
    REQUIRE(da.action == "drop");

    // The drop-poll lazily erases the record: a further poll finds it gone, so
    // the parked template (which _exit(0)s on drop) does not loop.
    RuntimeTemplateAction after;
    std::string after_err;
    REQUIRE(!sup.template_poll(template_id, "tok-1", after, after_err));
    REQUIRE(after_err == "unknown template");

    // The runtime's status no longer advertises the dropped template.
    RuntimeStatus st;
    REQUIRE(sup.status("rt-1", st, error));
    REQUIRE(st.templates.empty());
}

static void test_drop_template_rejects_in_flight_restore_template() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual",
                                 boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", action.template_id,
                               1300, 1300, 1, error));

    RuntimeRestoreIntent intent;
    intent.runtime_id = "rt-1";
    intent.template_id = action.template_id;
    intent.target_generation = 2;
    REQUIRE(sup.begin_restore(intent, error));

    std::string drop_err;
    REQUIRE(!sup.drop_template(action.template_id, drop_err));
    REQUIRE(drop_err == "template restore in progress");

    RuntimeTemplateAction template_action;
    REQUIRE(sup.publish_restore("rt-1", error));
    REQUIRE(sup.template_poll(action.template_id, "tok-1", template_action,
                              error));
    REQUIRE(template_action.action == "restore");

    std::string abort_err;
    REQUIRE(sup.abort_restore("rt-1", abort_err));
}

// --- I2: snapshots are serialized per-runtime. While the first snapshot's
// pending state is still set (union state not yet durable), a second
// begin_snapshot is refused; after the daemon clears the pending state (end of
// snapshot_runtime), a second is accepted.
static void test_snapshots_serialized_per_runtime() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    req.timeout_ms = 1000;

    // First snapshot in flight through release_boundary_for_snapshot.
    std::string op_id1;
    REQUIRE(sup.begin_snapshot(req, op_id1, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));

    // After release, has_pending_snapshot MUST still be true (snapshot not
    // durable), so a concurrent second begin_snapshot is refused.
    std::string op_id2;
    std::string err2;
    REQUIRE(!sup.begin_snapshot(req, op_id2, err2));
    REQUIRE(err2 == "snapshot already in progress");

    // The daemon's snapshot_runtime clears the pending state after union state
    // is durable (here modeled by cancel_snapshot, which clears the fields).
    std::string cancel_err;
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));

    // Now a second snapshot is accepted with a fresh op id.
    std::string op_id3;
    REQUIRE(sup.begin_snapshot(req, op_id3, error));
    REQUIRE(op_id3 != op_id1);

    // Cleanup.
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));
}

// --- I4: a consumed boundary is erased, so boundaries_ does not grow
// unboundedly across repeated snapshots. Also exercises the dangling-reference
// discipline (no Boundary& held across cv_.wait_for).
static void test_boundary_erased_after_consume() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    req.timeout_ms = 1000;

    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(boundary_id, action, error));

    // The runtime handler consumes the action.
    RuntimeBoundaryAction got;
    REQUIRE(sup.wait_boundary_action(boundary_id, 1000, got, error));
    REQUIRE(got.action == "snapshot");

    // The boundary MUST have been erased on consume.
    RuntimeBoundaryAction again;
    std::string again_err;
    REQUIRE(!sup.wait_boundary_action(boundary_id, 50, again, again_err));
    REQUIRE(again_err == "unknown boundary");

    // Across N further snapshots each boundary is consumed and erased, so the
    // map does not accumulate one Boundary per snapshot.
    for (int i = 0; i < 5; ++i) {
        std::string cancel_err;
        sup.cancel_snapshot("rt-1", cancel_err);  // clear pending from the release
        REQUIRE(sup.begin_snapshot(req, op_id, error));
        std::string bid;
        REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", bid, error));
        RuntimeBoundaryAction a;
        REQUIRE(sup.release_boundary_for_snapshot(bid, a, error));
        RuntimeBoundaryAction g;
        REQUIRE(sup.wait_boundary_action(bid, 1000, g, error));
        // Already-consumed -> unknown boundary (erased).
        RuntimeBoundaryAction g2;
        std::string e2;
        REQUIRE(!sup.wait_boundary_action(bid, 50, g2, e2));
        REQUIRE(e2 == "unknown boundary");
    }

    // Final cleanup.
    std::string cancel_err;
    sup.cancel_snapshot("rt-1", cancel_err);
}

// --- I4 complement: an orphaned boundary is reaped on wait_boundary_action
// timeout. Previously the timeout path returned without erasing, so a later
// release_boundary_for_snapshot set action_set on a boundary with no consumer
// and leaked one Boundary per such race forever. Now the waiter owns cleanup on
// timeout (it created the boundary via observe_boundary), so every boundary is
// erased either on consume or on timeout.
static void test_orphaned_boundary_reaped_on_timeout() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    req.timeout_ms = 1000;

    // observe_boundary creates the boundary; wait_boundary_action with a tiny
    // timeout and NO releaser must time out AND reap the record.
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", boundary_id, error));
    RuntimeBoundaryAction action;
    std::string wait_err;
    bool ok = sup.wait_boundary_action(boundary_id, 30, action, wait_err);
    REQUIRE(!ok);
    REQUIRE(wait_err == "snapshot timeout");

    // The boundary was reaped: a subsequent release returns "unknown boundary"
    // instead of setting action_set on an orphan and leaking the record.
    RuntimeBoundaryAction rel_action;
    std::string rel_err;
    REQUIRE(!sup.release_boundary_for_snapshot(boundary_id, rel_action, rel_err));
    REQUIRE(rel_err == "unknown boundary");
    // release_boundary_with_error must tolerate the reaped boundary too.
    RuntimeBoundaryAction err_action;
    std::string err_err;
    REQUIRE(!sup.release_boundary_with_error(boundary_id, "late", err_action,
                                              err_err));
    REQUIRE(err_err == "unknown boundary");

    // Clean up the pending snapshot (cancel_snapshot's boundary-release path
    // must tolerate the already-reaped boundary).
    std::string cancel_err;
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));
    REQUIRE(cancel_err.empty());

    // Loop N times: each snapshot times out and reaps its boundary, so the map
    // does not accumulate one Boundary per snapshot. We prove non-growth by
    // showing every release after each timeout fails with "unknown boundary"
    // (the record was reaped, not left behind for a late releaser to find).
    for (int i = 0; i < 5; ++i) {
        std::string op;
        REQUIRE(sup.begin_snapshot(req, op, error));
        std::string bid;
        REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", bid, error));
        RuntimeBoundaryAction a;
        std::string e;
        REQUIRE(!sup.wait_boundary_action(bid, 20, a, e));
        REQUIRE(e == "snapshot timeout");
        RuntimeBoundaryAction ra;
        std::string re;
        REQUIRE(!sup.release_boundary_for_snapshot(bid, ra, re));
        REQUIRE(re == "unknown boundary");
        std::string ce;
        REQUIRE(sup.cancel_snapshot("rt-1", ce));
    }
}

// --- Bundled: timeout coverage for the three previously-untested waits.
// wait_for_boundary / wait_for_template_ready / wait_template_published each
// return false with "snapshot timeout" when nothing satisfies the predicate.
static void test_blocking_timeouts_all_three_waits() {
    FakeProcessController pc;
    RuntimeSupervisor sup(&pc);
    std::string error;
    REQUIRE(sup.register_runtime(create_request(), error));

    RuntimeSnapshotRequest req;
    req.runtime_id = "rt-1";
    req.boundary_kind = "manual";
    req.timeout_ms = 1000;

    // wait_for_boundary: no observe_boundary fires -> "snapshot timeout".
    std::string op_id;
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string boundary_id;
    std::string b_err;
    REQUIRE(!sup.wait_for_boundary("rt-1", 50, boundary_id, b_err));
    REQUIRE(b_err == "snapshot timeout");
    std::string cancel_err;
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));

    // wait_for_template_ready: release sets pending_template_id but no
    // template_ready fires -> "snapshot timeout".
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string bid;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", bid, error));
    RuntimeBoundaryAction action;
    REQUIRE(sup.release_boundary_for_snapshot(bid, action, error));
    std::string t_err;
    REQUIRE(!sup.wait_for_template_ready("rt-1", 50, t_err));
    REQUIRE(t_err == "snapshot timeout");
    // Cleanup the half-prepared template + pending state.
    std::string drop_err;
    REQUIRE(sup.drop_template(action.template_id, drop_err));
    REQUIRE(sup.cancel_snapshot("rt-1", cancel_err));

    // wait_template_published: template ready but never published/errored ->
    // "snapshot timeout".
    REQUIRE(sup.begin_snapshot(req, op_id, error));
    std::string bid2;
    REQUIRE(sup.observe_boundary("rt-1", "tok-1", 1200, 1, "manual", bid2, error));
    RuntimeBoundaryAction action2;
    REQUIRE(sup.release_boundary_for_snapshot(bid2, action2, error));
    REQUIRE(sup.template_ready("rt-1", "tok-1", action2.template_id, 1300, 1300, 1, error));
    std::string p_err;
    REQUIRE(!sup.wait_template_published(action2.template_id, 50, p_err));
    REQUIRE(p_err == "snapshot timeout");
}

int main() {
    std::printf("test_runtime_supervisor:\n");
    test_register_and_status();
    test_pending_snapshot_boundary_and_template_ready();
    test_template_loss_degrades_status();
    test_restore_terminates_active_generation_and_waits_for_template();
    test_lifecycle_rejects_invalid_control_tokens();
    test_boundary_rendezvous_two_threads();
    test_blocking_timeouts();
    test_template_ready_rendezvous_two_threads();
    test_fail_template_publish_releases_blocked_waiter();
    test_cancel_snapshot_restores_snapshot_ability();
    test_late_generation_ready_after_abort_is_rejected();
    test_stale_generation_ready_for_wrong_generation_is_rejected();
    test_drop_template_makes_poll_return_drop();
    test_drop_template_rejects_in_flight_restore_template();
    test_snapshots_serialized_per_runtime();
    test_boundary_erased_after_consume();
    test_orphaned_boundary_reaped_on_timeout();
    test_blocking_timeouts_all_three_waits();
    std::printf("PASS test_runtime_supervisor\n");
    return 0;
}
