#pragma once
#include "agent_state_service.h"
#include "bootstrap.h"
#include "branch_context.h"
#include "branch_merge.h"
#include "branch_router.h"
#include "checkpoint.h"
#include "inode_map.h"
#include "object_store.h"
#include "policy_installer.h"
#include "refs.h"
#include "runtime_process_posix.h"
#include "runtime_supervisor.h"
#include "working_tree.h"
#include "write_buffer.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cas {

class TelemetryRegistry;
class CgroupWatch;

struct FhState {
    std::string path;
    uint32_t branch_id = 0;
    Hash pinned_commit;
    Hash base_blob;
    uint64_t base_size = 0;
    std::vector<uint8_t> base_cache;
    bool base_cache_loaded = false;
    std::unique_ptr<WriteBuffer> write_buf;
    bool stale = false;
    // Linux metadata optimization (Task 4): a read-capable, non-truncating
    // open retains an fd-backed BlobView so later getattr/read can report
    // size (and, in Task 6, serve reads) without re-materializing the blob.
    // Default BlobView is fd=-1 (invalid); macOS/Windows adapters never
    // touch these fields, so they remain unaffected. BlobView is move-only
    // and default-constructible, so FhState stays moveable via generated
    // move ops (no custom destructor/move added here).
    BlobView blob_view;
    bool fd_read_eligible = false;
};

struct BranchMergeResult {
    bool ok = false;
    Hash commit_hash = ZERO_HASH;
    std::vector<std::string> conflicts;
    std::string error;
};

class Daemon {
public:
    Daemon(std::string source_root,
           std::string mount_point,
           std::string store_root);
    // Out-of-line because registry_ is std::unique_ptr<TelemetryRegistry>
    // with TelemetryRegistry forward-declared above; the destructor needs
    // the complete type.
    ~Daemon();

    bool initialize();

    ObjectStore& store() { return store_; }
    Refs& refs() { return refs_; }
    // Safe convenience accessors pointing at the (permanent) main branch —
    // main_branch_ is never reassigned, so these references are stable for
    // Daemon's lifetime.
    WorkingTree& working_tree() { return main_branch_->wt; }
    CheckpointManager& checkpoint_mgr() { return cm_; }
    std::mutex& checkpoint_mutex() { return main_branch_->checkpoint_mu; }
    InodeMap& inode_map() { return inode_map_; }
    Bootstrap* bootstrap() { return bootstrap_.get(); }
    void set_bootstrap(std::unique_ptr<Bootstrap> bs) { bootstrap_ = std::move(bs); }
    // Daemon takes ownership of the TelemetryRegistry so its lifetime is
    // bounded by the Daemon's. shutdown_telemetry() (or ~Daemon as a
    // fallback) calls stop_all() before the registry is destroyed, ensuring
    // FUSE/probe threads cannot be calling into a half-destroyed registry.
    void install_registry(std::unique_ptr<TelemetryRegistry> registry);
    // Stop the registry and release it. Idempotent. Call this before FUSE
    // shutdown if you want to control the order; otherwise ~Daemon will do
    // it. Required because main.cpp must stop the control socket first
    // (commit bf4a933) and the registry second.
    void shutdown_telemetry();
    TelemetryRegistry* registry() { return registry_.get(); }
    const TelemetryRegistry* registry() const { return registry_.get(); }
    void set_policy_installer(PolicyInstaller* pi) { policy_installer_ = pi; }
    PolicyInstaller* policy_installer() { return policy_installer_; }
    const PolicyInstaller* policy_installer() const { return policy_installer_; }
    // Owned by main.cpp (like the routing fence); null when the cgroup
    // delete watch could not start. Control handlers add/remove watches
    // around register/unregister when present.
    void set_cgroup_watch(CgroupWatch* watch) { cgroup_watch_ = watch; }
    CgroupWatch* cgroup_watch() { return cgroup_watch_; }

    // Cooperative-runtime supervisor + the daemon-coupled snapshot/restore
    // orchestrations. snapshot_runtime/restore_runtime drive the blocking
    // rendezvous through the supervisor and couple it with CheckpointManager
    // (branch checkpoint for snapshots, rollback for restores).
    RuntimeSupervisor& runtime_supervisor() { return runtime_supervisor_; }
    const RuntimeSupervisor& runtime_supervisor() const { return runtime_supervisor_; }
    RuntimeSnapshotResult snapshot_runtime(const RuntimeSnapshotRequest& request);
    // timeout_ms bounds the wait_for_generation_ready rendezvous; it defaults to
    // 5000 so existing callers (and the system test, which restores with no
    // --timeout-ms) are unchanged. A client may override it via the
    // runtime.restore handler's optional "timeout_ms" field.
    RuntimeRestoreResult restore_runtime(const std::string& union_state_id_hex,
                                         uint64_t timeout_ms = 5000);

    // Owns the agent-state lifecycle (append/describe/latest/restore_session)
    // on top of Task 1's record helpers. Constructed against the ObjectStore
    // and only used after initialize() has run init_layout(); control commands
    // are never dispatched before initialize() completes.
    AgentStateService& agent_state() { return agent_state_; }
    const AgentStateService& agent_state() const { return agent_state_; }

    // Reusable branch-rollback orchestration: resolves the branch under the
    // branch checkpoint lock, calls CheckpointManager::rollback_locked against
    // the target commit, and invalidates file handles for the target branch.
    // Both the existing `rollback` control command and `state.restore
    // mode=full` route through this helper so locking and FH-invalidation are
    // defined in exactly one place. The target is a resolved commit Hash; the
    // `rollback` command resolves its label/hash string first (via
    // CheckpointManager::resolve_target) and `state.restore mode=full` passes
    // the record's fs_commit directly.
    RollbackResult rollback_branch_to_commit(const std::string& branch_name,
                                             const Hash& target);

    // All branch accessors return a std::shared_ptr so the caller keeps the
    // BranchContext alive for the duration of the op, regardless of
    // concurrent delete_branch calls. This closes the use-after-free race
    // where a raw BranchContext* could be freed between lookup and use.
    std::shared_ptr<BranchContext> branch(uint32_t id);
    std::shared_ptr<BranchContext> branch_by_name(const std::string& name);
    std::shared_ptr<BranchContext> branch_by_name_locked(
        const std::string& name,
        std::unique_lock<std::mutex>& checkpoint_lock);
    std::shared_ptr<BranchContext> main_branch() const { return main_branch_; }
    std::shared_ptr<BranchContext> branch_for_pid(Pid pid);
    BranchRouter& router() { return router_; }

    CheckpointResult checkpoint_branch(
        const std::shared_ptr<BranchContext>& branch,
        const std::string& label);
    CheckpointResult checkpoint_branch_by_name(
        const std::string& branch_name,
        const std::string& label);

    uint32_t create_branch(const std::string& name, const std::string& from);
    BranchMergeResult merge_branch(const std::string& source_name,
                                   const std::string& target_name,
                                   const std::string& label);
    bool delete_branch(const std::string& name);
    std::vector<std::shared_ptr<BranchContext>> list_branches();

    const std::string& source_root() const { return source_root_; }
    const std::string& mount_point() const { return mount_point_; }
    const std::string& store_root() const { return store_root_; }
    uint64_t session_id() const { return session_id_; }
    uint32_t policy_version() const { return policy_version_.load(); }
    void bump_policy_version() { policy_version_.fetch_add(1); }

    uint64_t allocate_fh(std::unique_ptr<FhState> state);
    std::shared_ptr<FhState> get_fh(uint64_t fh);
    void release_fh(uint64_t fh);
    bool flush_fhs_for_branch(uint32_t branch_id);
    bool flush_fhs_for_branch(uint32_t branch_id, WorkingTree& wt);
    void invalidate_fhs_for_branch(uint32_t branch_id);

    // Returns the effective size of an open, dirty fh for `path` on the
    // given branch if one exists (read-your-writes through fstat/stat
    // when kernel calls getattr without passing fi->fh). Returns -1 if
    // none found. branch_id is required — no default, to avoid silently
    // routing new callers to main.
    int64_t dirty_fh_size_for_path(const std::string& path, uint32_t branch_id);

    void ensure_base_cache(FhState& s);

    static bool is_hidden(const std::string& path);

private:
    bool rebuild_branch_from_ref(const std::string& name,
                                 uint32_t branch_id,
                                 std::shared_ptr<BranchContext>& out,
                                 std::string& error);
    bool rebuild_main_from_ref(std::string& error);
    void load_persisted_branches();

    std::string source_root_;
    std::string mount_point_;
    std::string store_root_;

    ObjectStore store_;
    Refs refs_;
    CheckpointManager cm_;
    // Declared after cm_ (which itself follows store_) so it constructs after
    // the ObjectStore it references and destructs before it. Member init-list
    // position mirrors this declaration order.
    AgentStateService agent_state_;
    InodeMap inode_map_;
    std::unique_ptr<Bootstrap> bootstrap_;

    std::mutex fh_table_mu_;
    std::unordered_map<uint64_t, std::shared_ptr<FhState>> fh_table_;
    std::atomic<uint64_t> next_fh_id_{1};

    mutable std::shared_mutex branches_mu_;
    std::map<uint32_t, std::shared_ptr<BranchContext>> branches_;
    std::unordered_map<std::string, uint32_t> branch_name_ids_;
    std::unordered_set<std::string> branch_create_reservations_;
    std::unordered_map<std::string, uint32_t> branch_source_reservations_;
    // Permanent shared_ptr to the main branch (branch_id 0). Main is never
    // deleted, so this pointer stays valid for Daemon's entire lifetime and
    // working_tree() / checkpoint_mutex() below are safe to return references
    // into main_branch_->wt and main_branch_->checkpoint_mu respectively.
    std::shared_ptr<BranchContext> main_branch_;
    std::atomic<uint32_t> next_branch_id_{1};
    BranchRouter router_;

    uint64_t session_id_ = 0;
    std::atomic<uint32_t> policy_version_{1};

    // Cooperative-runtime ownership. The controller is declared BEFORE the
    // supervisor so it constructs first; the supervisor holds a raw pointer
    // to it. Both precede registry_ so they outlive any telemetry backend
    // that may capture `daemon`.
    PosixRuntimeProcessController runtime_process_controller_;
    RuntimeSupervisor runtime_supervisor_;

    // Owned. Declared LAST so it is destroyed FIRST (members destruct in
    // reverse declaration order). That gets the registry torn down before
    // any of the other Daemon members it might reference (store_,
    // inode_map_, branches_, bootstrap_) — necessary because backends can
    // hold callbacks that capture `daemon`. ~Daemon also calls
    // shutdown_telemetry() defensively in case main.cpp didn't, and an
    // explicit shutdown_telemetry() call lets main.cpp order the registry
    // stop relative to FUSE / control-socket shutdown (commit bf4a933).
    std::unique_ptr<TelemetryRegistry> registry_;
    PolicyInstaller* policy_installer_ = nullptr;
    CgroupWatch* cgroup_watch_ = nullptr;
};

} // namespace cas
