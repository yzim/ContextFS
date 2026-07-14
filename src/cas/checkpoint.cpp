#include "checkpoint.h"
#include "tree_serialize.h"
#include "write_buffer.h"
#include <chrono>
#include <cstring>
#include <unordered_set>

namespace cas {

namespace {

struct HashHasher {
    size_t operator()(const Hash& h) const noexcept {
        size_t v;
        std::memcpy(&v, h.data(), sizeof(v));
        return v;
    }
};

// Filter `pending` down to objects that are part of the committed graph for
// this checkpoint: the WT's leaf blobs/symlinks, the tree objects we just
// wrote, and the commit. Anything else in `pending` is an orphan (e.g. a
// blob that was written and then overwritten before this checkpoint) and
// does not need to be made durable — it isn't reachable from any ref.
std::vector<Hash> filter_to_committed_graph(
    const std::vector<Hash>& pending,
    const WorkingTree& wt,
    const std::vector<Hash>& written_tree_hashes,
    const Hash& commit_hash) {

    std::unordered_set<Hash, HashHasher> referenced;
    referenced.reserve(pending.size() * 2);

    wt.for_each_including_deleted([&](const std::string&,
                                       const WorkingTreeEntry& e) {
        if ((e.kind == EntryKind::Blob || e.kind == EntryKind::Symlink) &&
            e.hash != ZERO_HASH) {
            referenced.insert(e.hash);
        }
    });
    for (const auto& h : written_tree_hashes) referenced.insert(h);
    referenced.insert(commit_hash);

    std::vector<Hash> out;
    out.reserve(pending.size());
    for (const auto& h : pending) {
        if (referenced.count(h)) out.push_back(h);
    }
    return out;
}

} // namespace

CheckpointManager::CheckpointManager(ObjectStore& store, Refs& refs)
    : store_(store), refs_(refs) {}

CheckpointResult CheckpointManager::checkpoint_locked(
    const std::string& label,
    uint64_t session_id,
    uint32_t policy_version,
    WorkingTree& wt,
    const std::function<bool()>& flush_fn,
    const std::string& branch_name) {

    // Fix I5: abort before advancing refs if any open fh failed to flush.
    if (!flush_fn()) return {false, ZERO_HASH, "flush failed"};

    std::vector<Hash> written_hashes;
    std::string serialize_error;
    Hash tree_hash = serialize_working_tree(
        wt, store_, written_hashes, nullptr, &serialize_error);
    if (tree_hash == ZERO_HASH)
        return {false, ZERO_HASH,
                serialize_error.empty()
                    ? "failed to serialize working tree"
                    : "failed to serialize working tree: " + serialize_error};

    Hash parent;
    bool has_parent = refs_.read_ref(branch_name, parent);

    CommitData cd;
    cd.tree_hash = tree_hash;
    if (has_parent) cd.parents.push_back(parent);
    cd.session_id = session_id;
    cd.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    cd.label = label;
    cd.policy_version = policy_version;

    auto commit_body = serialize_commit(cd);
    Hash commit_hash = store_.write_commit(commit_body);
    if (commit_hash == ZERO_HASH)
        return {false, ZERO_HASH, "failed to write commit object"};

    // Drain pending writes, then narrow to objects reachable from this
    // commit. Old leaves from prior checkpoints aren't in `pending` (already
    // drained and made durable). Orphans created by intra-window overwrites
    // are in `pending` but not referenced — they're skipped.
    std::vector<Hash> pending = store_.drain_pending();
    std::vector<Hash> to_fsync = filter_to_committed_graph(
        pending, wt, written_hashes, commit_hash);
    if (!store_.fsync_objects(to_fsync)) {
        store_.restore_pending(pending);
        return {false, ZERO_HASH, "failed to fsync checkpoint objects"};
    }
    if (!store_.fsync_shard_dirs(to_fsync)) {
        store_.restore_pending(pending);
        return {false, ZERO_HASH, "failed to fsync checkpoint shard dirs"};
    }

    if (!refs_.write_ref(branch_name, commit_hash, store_.tmp_dir()))
        return {false, ZERO_HASH, "failed to advance refs/" + branch_name};

    return {true, commit_hash, ""};
}

CheckpointResult CheckpointManager::checkpoint(
    const std::string& label,
    uint64_t session_id,
    uint32_t policy_version,
    std::mutex& checkpoint_mutex,
    WorkingTree& wt,
    const std::function<bool()>& flush_fn,
    const std::string& branch_name) {

    std::lock_guard<std::mutex> lk(checkpoint_mutex);
    return checkpoint_locked(label, session_id, policy_version, wt, flush_fn,
                             branch_name);
}

Hash CheckpointManager::resolve_target(const std::string& target,
                                        const std::string& branch_name) {
    if (target.size() >= 4) {
        Hash h;
        if (target.size() == 64 && hex_to_hash(target.c_str(), h)) {
            if (store_.object_exists(h)) return h;
        }
    }

    Hash current;
    if (!refs_.read_ref(branch_name, current)) return ZERO_HASH;

    Hash cursor = current;
    for (int depth = 0; depth < 100000; depth++) {
        std::vector<uint8_t> body;
        if (!store_.read_commit(cursor, body)) break;
        CommitData cd;
        if (!deserialize_commit(body, cd)) break;
        if (cd.label == target) return cursor;
        if (cd.parents.empty()) break;
        cursor = cd.parents[0];
    }
    return ZERO_HASH;
}

RollbackResult CheckpointManager::rollback(
    const std::string& target,
    std::mutex& checkpoint_mutex,
    WorkingTree& wt,
    const std::function<void()>& invalidate_fhs_fn,
    const std::string& branch_name) {

    std::lock_guard<std::mutex> lk(checkpoint_mutex);
    return rollback_locked(target, wt, invalidate_fhs_fn, branch_name);
}

RollbackResult CheckpointManager::rollback_locked(
    const std::string& target,
    WorkingTree& wt,
    const std::function<void()>& invalidate_fhs_fn,
    const std::string& branch_name) {

    Hash target_hash = resolve_target(target, branch_name);
    if (target_hash == ZERO_HASH)
        return {false, ZERO_HASH, "target commit not found"};

    std::vector<uint8_t> body;
    if (!store_.read_commit(target_hash, body))
        return {false, ZERO_HASH, "failed to read target commit"};
    CommitData cd;
    if (!deserialize_commit(body, cd))
        return {false, ZERO_HASH, "failed to parse target commit"};

    std::string rebuild_error;
    if (!rebuild_working_tree(cd.tree_hash, store_, wt, &rebuild_error)) {
        // A missing tree object under a resolvable commit means retention
        // swept this snapshot's data (spec Part 2 normative error).
        if (rebuild_error.rfind("tree object missing", 0) == 0)
            return {false, ZERO_HASH, "checkpoint compacted by retention policy"};
        return {false, ZERO_HASH, "failed to rebuild working tree: " + rebuild_error};
    }

    invalidate_fhs_fn();

    if (!refs_.write_ref(branch_name, target_hash, store_.tmp_dir()))
        return {false, ZERO_HASH, "failed to write refs/" + branch_name};

    return {true, target_hash, ""};
}

Hash CheckpointManager::current_commit(const std::string& branch_name) const {
    Hash h;
    if (refs_.read_ref(branch_name, h)) return h;
    return ZERO_HASH;
}

} // namespace cas
