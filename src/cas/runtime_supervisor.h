#pragma once

#include "runtime_state.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cas {

struct RuntimeCreateRequest {
    std::string runtime_id;
    std::string branch = "main";
    int64_t root_pid = -1;
    int64_t process_group_id = -1;
    std::string command_ref;
    std::string cwd;
    bool cooperative = false;
    std::string control_token;
};

struct TemplateStatus {
    std::string template_id;
    std::string runtime_id;
    int64_t template_pid = -1;
    int64_t template_process_group_id = -1;
    uint64_t generation = 0;
    bool alive = false;
    std::string union_state_id;
    Hash fs_commit = ZERO_HASH;
    RestoreEligibility restore_eligibility = RestoreEligibility::MetadataOnly;
};

struct RuntimeStatus {
    std::string runtime_id;
    std::string branch;
    int64_t root_pid = -1;
    int64_t active_process_group_id = -1;
    uint64_t generation = 0;
    std::string command_ref;
    bool cooperative = false;
    RestoreEligibility restore_eligibility = RestoreEligibility::MetadataOnly;
    std::vector<TemplateStatus> templates;
};

struct RuntimeSnapshotRequest {
    std::string runtime_id;
    std::string boundary_kind = "manual";
    std::string agent_state_id;
    uint64_t timeout_ms = 1000;
};

struct RuntimeBoundaryAction {
    std::string action = "continue";
    std::string operation_id;
    std::string template_id;
    std::string error;
};

struct RuntimeTemplateAction {
    std::string action = "wait";
    uint64_t target_generation = 0;
};

struct RuntimeRestoreIntent {
    std::string runtime_id;
    std::string template_id;
    uint64_t target_generation = 0;
};

struct RuntimeSnapshotResult {
    bool ok = false;
    std::string runtime_id;
    std::string template_id;
    std::string union_state_id;
    Hash fs_commit = ZERO_HASH;
    uint64_t generation = 0;
    RestoreEligibility restore_eligibility = RestoreEligibility::MetadataOnly;
    std::string error;
};

struct RuntimeRestoreResult {
    bool ok = false;
    std::string runtime_id;
    std::string template_id;
    uint64_t target_generation = 0;
    Hash fs_commit = ZERO_HASH;
    RestoreEligibility restore_eligibility = RestoreEligibility::MetadataOnly;
    std::string partial;
    std::string error;
};

class RuntimeProcessController {
public:
    virtual ~RuntimeProcessController() = default;
    virtual bool supported(std::string& error) const {
        error.clear();
        return true;
    }
    virtual bool process_alive(int64_t pid) = 0;
    virtual bool freeze_process_group(int64_t pgid, std::string& error) = 0;
    virtual bool resume_process_group(int64_t pgid, std::string& error) = 0;
    virtual bool terminate_process_group(int64_t pgid, std::string& error) = 0;
};

// RuntimeSupervisor is a thread-safe in-memory registry of cooperative
// runtimes, their templates, snapshot boundaries and restore intents. It does
// NOT call CheckpointManager; that coupling belongs in Daemon.
//
// Two kinds of threads rendezvous through the supervisor:
//   - daemon snapshot/restore-requester thread: drives a snapshot or restore
//     and blocks on the wait_for_* methods below.
//   - per-connection control-socket handler threads: the cooperative runtime
//     calls observe_boundary / template_ready / generation_ready on these and
//     blocks in wait_boundary_action / wait_template_published until released.
class RuntimeSupervisor {
public:
    explicit RuntimeSupervisor(RuntimeProcessController* process_controller);
    bool supported(std::string& error) const;

    // --- registry --------------------------------------------------------
    bool register_runtime(const RuntimeCreateRequest& request,
                          std::string& error);
    bool status(const std::string& runtime_id,
                RuntimeStatus& out,
                std::string& error);
    std::vector<RuntimeStatus> list();

    // --- snapshot (non-blocking core) ------------------------------------
    bool begin_snapshot(const RuntimeSnapshotRequest& request,
                        std::string& operation_id,
                        std::string& error);
    // Clears any pending snapshot for runtime_id (e.g. after wait_for_boundary
    // timed out) so begin_snapshot can be accepted again. If a boundary was
    // already observed for the pending op, releases that boundary handler with
    // an error so it does not park forever inside wait_boundary_action.
    // Returns false only when the runtime is unknown.
    bool cancel_snapshot(const std::string& runtime_id,
                         std::string& error);
    // Non-blocking. Creates a boundary record and returns a boundary id when a
    // pending snapshot matches; notifies any wait_for_boundary waiter. Returns
    // false with error="no pending snapshot" when nothing matches.
    bool observe_boundary(const std::string& runtime_id,
                          const std::string& control_token,
                          int64_t pid,
                          uint64_t generation,
                          const std::string& boundary_kind,
                          std::string& boundary_id,
                          std::string& error);
    // Sets the boundary action to "snapshot" (allocating a fresh template id)
    // and notifies wait_boundary_action.
    bool release_boundary_for_snapshot(const std::string& boundary_id,
                                       RuntimeBoundaryAction& out,
                                       std::string& error);
    // Sets the boundary action to "error" with the given message and notifies
    // wait_boundary_action.
    bool release_boundary_with_error(const std::string& boundary_id,
                                     const std::string& message,
                                     RuntimeBoundaryAction& out,
                                     std::string& error);
    // Non-blocking. Records template liveness and notifies
    // wait_for_template_ready waiters.
    bool template_ready(const std::string& runtime_id,
                        const std::string& control_token,
                        const std::string& template_id,
                        int64_t template_pid,
                        int64_t template_process_group_id,
                        uint64_t generation,
                        std::string& error);
    bool template_status(const std::string& template_id,
                         TemplateStatus& out,
                         std::string& error);
    bool restore_template_status(const std::string& runtime_id,
                                 TemplateStatus& out,
                                 std::string& error);
    // Sets the published union state id / fs commit for a template and
    // notifies wait_template_published.
    bool attach_union_state(const std::string& template_id,
                            const std::string& union_state_id,
                            const Hash& fs_commit,
                            std::string& error);
    // Marks the template's publish as permanently failed with `message` and
    // releases any wait_template_published waiter with that error. Unlike
    // drop_template, does NOT erase the template (so a blocked waiter can
    // still re-inspect it under the lock). Returns false only when the
    // template is unknown.
    bool fail_template_publish(const std::string& template_id,
                               const std::string& message,
                               std::string& error);

    // --- restore (non-blocking core) -------------------------------------
    bool begin_restore(const RuntimeRestoreIntent& intent,
                       std::string& error);
    // Releases a restore intent to the parked template. begin_restore() freezes
    // and records intent, but template_poll() must not return action:"restore"
    // until Daemon has rolled the filesystem back and invalidated handles.
    bool publish_restore(const std::string& runtime_id,
                         std::string& error);
    // Restore-failure cleanup: resumes the frozen active process group via the
    // process controller and clears restore-pending state + the template
    // restore intent. Mirrors generation_ready's lock discipline (resume
    // outside mu_). Returns false only when the runtime is unknown.
    bool abort_restore(const std::string& runtime_id,
                       std::string& error);
    bool template_poll(const std::string& template_id,
                       const std::string& control_token,
                       RuntimeTemplateAction& out,
                       std::string& error);
    // Records the restored generation and stashes the previously-active
    // process group for retire_pending_generation(). Notifies
    // wait_for_generation_ready. Does NOT terminate the prior group itself: the
    // restored grandchild blocks on this call's acknowledgement, so old-
    // generation cleanup is deferred to restore_runtime().
    bool generation_ready(const std::string& runtime_id,
                          const std::string& control_token,
                          int64_t pid,
                          int64_t active_process_group_id,
                          uint64_t generation,
                          std::string& error);
    // Terminates the process group stashed by generation_ready() (the prior
    // active generation). Called by Daemon::restore_runtime() after
    // wait_for_generation_ready() succeeds, so the kill blocks the restore
    // requester (not the restored runtime's ready ack). Returns false only when
    // the runtime is unknown.
    bool retire_pending_generation(const std::string& runtime_id,
                                   std::string& error);
    bool drop_template(const std::string& template_id,
                       std::string& error);

    // --- blocking coordination (daemon side) -----------------------------
    // Block until the cooperative runtime observes the boundary for the pending
    // snapshot of runtime_id, or timeout. On success sets boundary_id.
    // Timeout -> false, error = "snapshot timeout".
    bool wait_for_boundary(const std::string& runtime_id,
                           uint64_t timeout_ms,
                           std::string& boundary_id,
                           std::string& error);
    // Block until the parked template reports ready for runtime_id, or
    // timeout. Timeout -> false, error = "snapshot timeout".
    bool wait_for_template_ready(const std::string& runtime_id,
                                 uint64_t timeout_ms,
                                 std::string& error);
    // Block until the restored generation reports ready for runtime_id, or
    // timeout. Timeout -> false, error = "restore timeout".
    bool wait_for_generation_ready(const std::string& runtime_id,
                                   uint64_t timeout_ms,
                                   std::string& error);

    // --- blocking coordination (control-socket handler side) -------------
    // Block the runtime.boundary handler until snapshot_runtime releases the
    // boundary. Timeout -> false, error = "snapshot timeout".
    bool wait_boundary_action(const std::string& boundary_id,
                              uint64_t timeout_ms,
                              RuntimeBoundaryAction& out,
                              std::string& error);
    // Block the runtime.template.ready handler until snapshot_runtime attaches
    // the union state. Timeout -> false, error = "snapshot timeout".
    bool wait_template_published(const std::string& template_id,
                                 uint64_t timeout_ms,
                                 std::string& error);

private:
    // In-memory record for one registered runtime. Private implementation
    // detail (defined here so std::map value types are complete).
    struct Runtime {
        std::string runtime_id;
        std::string branch = "main";
        int64_t root_pid = -1;
        int64_t active_process_group_id = -1;
        uint64_t generation = 0;
        std::string command_ref;
        std::string cwd;
        bool cooperative = false;
        std::string control_token;
        std::vector<std::string> template_ids;

        // Pending snapshot for this runtime (at most one in flight).
        bool has_pending_snapshot = false;
        std::string pending_op_id;
        std::string pending_boundary_kind;
        std::string pending_agent_state_id;
        uint64_t pending_timeout_ms = 0;
        std::string pending_boundary_id;  // set when the runtime observes boundary
        bool boundary_observed = false;

        // Template currently being prepared (set when release_boundary_for_snapshot
        // allocates a template id; consumed by the daemon's template-ready wait).
        std::string pending_template_id;

        // Pending restore for this runtime (at most one in flight).
        bool has_pending_restore = false;
        std::string restore_template_id;
        uint64_t restore_target_generation = 0;

        // Signal set by generation_ready(), consumed by wait_for_generation_ready().
        bool generation_ready_flag = false;

        // Prior active process group id stashed by generation_ready() (the new
        // generation's ready ack) for restore_runtime() to retire via
        // retire_pending_generation(). Decouples the ack from the blocking kill
        // so the restored grandchild resumes immediately.
        int64_t retire_pgid = 0;
    };

    struct Boundary {
        std::string boundary_id;
        std::string runtime_id;
        std::string operation_id;
        int64_t pid = -1;
        uint64_t generation = 0;
        std::string boundary_kind;
        std::string template_id;  // allocated at release time
        bool action_set = false;
        RuntimeBoundaryAction action;
    };

    struct Template {
        std::string template_id;
        std::string runtime_id;
        int64_t template_pid = -1;
        int64_t template_process_group_id = -1;
        uint64_t generation = 0;
        bool ready = false;       // template_ready() called
        bool published = false;   // attach_union_state() called
        std::string publish_error;
        std::string union_state_id;
        Hash fs_commit = ZERO_HASH;
        bool restore_pending = false;  // begin_restore() parked an action
        bool restore_released = false;  // publish_restore() made it pollable
        // Guards the one hand-off of a pending restore to the template's poll
        // loop. Set when template_poll first returns action:"restore" for a
        // pending intent; without it the template (polling every few ms) would
        // fork a second grandchild before the first one reached
        // generation_ready() and cleared restore_pending. Cleared together with
        // restore_pending by generation_ready() / abort_restore(), and re-armed
        // (to false) by begin_restore().
        bool restore_in_flight = false;
        uint64_t target_generation = 0;
        // Set by drop_template() and consumed by template_poll(): the parked
        // template's next poll must observe action:"drop" and _exit(0), rather
        // than spinning on {"ok":false,"error":"unknown template"} after the
        // record is removed. Erased lazily by template_poll() once the drop
        // action has been read, so the record does not leak.
        bool dropped = false;
    };

    void build_status_locked(const Runtime& r, RuntimeStatus& out) const;
    std::string make_runtime_id_locked();
    std::string make_template_id_locked();
    std::string make_boundary_id_locked();
    std::string make_operation_id_locked();

    // Non-owning. Owned by Daemon (constructed once at daemon startup and stored
    // for the daemon's lifetime). MUST outlive the RuntimeSupervisor instance:
    // build_status_locked / template_status / begin_restore / abort_restore /
    // retire_pending_generation dereference it without a null check. The
    // supervisor never frees or resets it.
    RuntimeProcessController* process_controller_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Runtime> runtimes_;
    std::map<std::string, Boundary> boundaries_;
    std::map<std::string, Template> templates_;
    uint64_t runtime_counter_ = 0;
    uint64_t template_counter_ = 0;
    uint64_t boundary_counter_ = 0;
    uint64_t operation_counter_ = 0;
};

} // namespace cas
