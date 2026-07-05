#pragma once
#include "commit.h"
#include "hash.h"
#include "object_store.h"
#include "refs.h"
#include "working_tree.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace cas {

struct CheckpointResult {
    bool ok;
    Hash commit_hash;
    std::string error;
};

struct RollbackResult {
    bool ok;
    Hash rolled_back_to;
    std::string error;
};

class CheckpointManager {
public:
    CheckpointManager(ObjectStore& store, Refs& refs);

    // flush_fn returns false on failure (e.g. write_blob error); the
    // checkpoint is aborted in that case rather than advancing the ref
    // over a corrupt tree.
    // Caller must already hold the branch checkpoint mutex; this helper
    // intentionally does not acquire it.
    CheckpointResult checkpoint_locked(
        const std::string& label,
        uint64_t session_id,
        uint32_t policy_version,
        WorkingTree& wt,
        const std::function<bool()>& flush_fn,
        const std::string& branch_name = "main");

    CheckpointResult checkpoint(
        const std::string& label,
        uint64_t session_id,
        uint32_t policy_version,
        std::mutex& checkpoint_mutex,
        WorkingTree& wt,
        const std::function<bool()>& flush_fn,
        const std::string& branch_name = "main");

    RollbackResult rollback(
        const std::string& target,
        std::mutex& checkpoint_mutex,
        WorkingTree& wt,
        const std::function<void()>& invalidate_fhs_fn,
        const std::string& branch_name = "main");

    // Caller must already hold the branch checkpoint mutex; this helper
    // intentionally does not acquire it.
    RollbackResult rollback_locked(
        const std::string& target,
        WorkingTree& wt,
        const std::function<void()>& invalidate_fhs_fn,
        const std::string& branch_name = "main");

    Hash current_commit(const std::string& branch_name = "main") const;

    // Resolves a rollback target string into a commit Hash. A 64-char hex hash
    // that names an existing object is returned as-is; any other string is
    // treated as a commit label and matched by walking the branch's first-parent
    // commit chain. Returns ZERO_HASH if nothing matches. Exposed so the
    // Daemon's rollback_branch_to_commit helper and the `rollback` control
    // command can share one resolution path instead of duplicating the
    // label walk.
    Hash resolve_target(const std::string& target,
                        const std::string& branch_name);

private:
    ObjectStore& store_;
    Refs& refs_;
};

} // namespace cas
