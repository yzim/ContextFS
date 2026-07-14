#pragma once

#include "agent_state.h"
#include "hash.h"
#include "object_store.h"

#include <cstddef>
#include <string>
#include <vector>

namespace cas {

// Inputs to AgentStateService::append(). The caller fills the record (including
// agent_id, branch, parent/snapshot anchors and payload fields). When `sync` is
// The service persists the stable, de-duplicated union of explicit
// dependency_hashes and the blob dependency derived from record.payload_ref.
// When `sync` is true, that same effective set is made durable alongside the
// state blob. `sync=false` appends remain logical-only: they are describable by
// `state_id` but do not publish a latest ref or fsync their dependencies.
struct AgentStateAppendRequest {
    AgentStateRecord record;
    std::vector<Hash> dependency_hashes;
    bool sync = false;
};

// Result of an append. On success `state_id` is the hex CAS hash of the
// published state blob and `durability` is "durable" (sync=true) or
// "logical_only" (sync=false). On failure `ok` is false and `error` carries a
// human-readable message; `state_id` may still be set if the state blob was
// written but a later publishing step failed.
struct AgentStateAppendResult {
    bool ok = false;
    std::string error;
    std::string state_id;
    std::string durability;
};

// Result of describe()/latest(). On success `record` carries the full
// deserialized state with `state_id` re-derived from the CAS hash.
struct AgentStateDescribeResult {
    bool ok = false;
    std::string error;
    AgentStateRecord record;
};

// Result of restore_session(). `chain` is ordered newest-first: it begins at
// the requested `state_id` and walks `parent_state_id` links back to (and
// including) the snapshot base, or to the session root if the record has no
// snapshot base anchor.
struct AgentStateRestoreResult {
    bool ok = false;
    std::string error;
    std::vector<AgentStateRecord> chain;
};

// Owns the agent-state lifecycle on top of Task 1's record helpers: appends
// state records to the CAS, publishes lightweight discovery refs under
// <store>/state/latest/<agent_id>/<branch>, and walks a bounded parent chain
// for session restore. State blobs remain the single source of truth; the
// latest refs and <store>/state/index.log are best-effort discovery metadata.
class AgentStateService {
public:
    explicit AgentStateService(ObjectStore& store);

    // Writes the record with its effective dependency set (and, when sync=true,
    // fsyncs those dependencies plus the state blob together) and publishes
    // the latest ref. Validates `agent_id`, `branch`, all hash-shaped fields,
    // and — for sync=true only — that parent_state_id and
    // snapshot_base_state_id are present and reference readable state objects.
    AgentStateAppendResult append(const AgentStateAppendRequest& req);

    // Loads a state by its hex CAS id.
    AgentStateDescribeResult describe(const std::string& state_id) const;

    // Resolves the most recently published durable state for an agent+branch.
    // Returns ok=false if no latest ref exists for the pair.
    AgentStateDescribeResult latest(const std::string& agent_id,
                                    const std::string& branch) const;

    // Walks the parent chain from `state_id` back to its snapshot base or root,
    // visiting at most `max_depth` records. Fails with a clear error when the
    // chain is longer than `max_depth`.
    AgentStateRestoreResult restore_session(const std::string& state_id,
                                            size_t max_depth) const;

private:
    ObjectStore& store_;
};

// Validates a state-ref path component (agent_id OR branch) against the plain
// character rule [A-Za-z0-9_-]{1,64}. The agent-state `branch` field is a LABEL
// referencing an existing VFS branch (it becomes the leaf filename of the ref
// path <store>/state/latest/<agent_id>/<branch>), NOT a branch being created,
// so the branch-name reserved-word list does NOT apply here: "main" (the
// default VFS branch and the default AgentStateRecord::branch), "HEAD",
// "objects" etc. are all legal state-ref labels. Path-safety follows from
// rejecting '/', '.', space and any character outside [A-Za-z0-9_-]. VFS-branch
// creation elsewhere still uses is_valid_branch_name() with its reserved words.
bool is_valid_state_ref_component(const std::string& name);

} // namespace cas
