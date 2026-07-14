#include "gc.h"
#include "agent_state.h"
#include "commit.h"
#include "runtime_state.h"
#include "tree_serialize.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace cas {

namespace fs = std::filesystem;

GcRunner::GcRunner(ObjectStore& store, Refs& refs) : store_(store), refs_(refs) {}

// Agent-state payload_ref is a typed CAS reference. Empty means unset; a
// non-empty malformed value is a corrupt state record and must fail closed.
static bool mark_required_hex_blob(const std::string& hex, GcMarkSet& marked,
                                   std::string& error) {
    if (hex.empty()) return true;
    Hash h;
    if (!hex_to_hash_strict(hex, h)) {
        error = "malformed object id: " + hex;
        return false;
    }
    marked.insert(h);
    return true;
}

// Union command/resource refs are opaque strings in normal records (argv:/,
// inline:, ...). A value is a CAS dependency only when the entire string is a
// strict 64-hex object id; every other value is intentionally ignored.
static void mark_opaque_cas_ref(const std::string& value, GcMarkSet& marked) {
    Hash h;
    if (hex_to_hash_strict(value, h)) marked.insert(h);
}

static void record_missing_hash(ObjectStore& store, const Hash& hash,
                                std::vector<Hash>* missing) {
    if (missing && !store.object_exists(hash)) missing->push_back(hash);
}

bool GcRunner::mark_tree_recursive(const Hash& tree, GcMarkSet& marked,
                                   MarkTraversal& traversal,
                                   std::string& error) {
    if (!traversal.trees.insert(tree).second) return true;
    marked.insert(tree);
    std::vector<uint8_t> body;
    if (!store_.read_tree(tree, body)) {
        record_missing_hash(store_, tree, traversal.missing);
        error = "mark: tree object missing: " + hash_to_hex(tree);
        return false;
    }
    std::vector<std::tuple<std::string, uint32_t, EntryKind, Hash>> entries;
    if (!deserialize_tree_entries(body, entries)) {
        error = "mark: corrupt tree object: " + hash_to_hex(tree);
        return false;
    }
    for (auto& [name, mode, kind, h] : entries) {
        (void)name; (void)mode;
        if (kind == EntryKind::Tree) {
            if (!mark_tree_recursive(h, marked, traversal, error)) return false;
        } else if (kind != EntryKind::Deleted && !(h == ZERO_HASH)) {
            marked.insert(h);
        }
    }
    return true;
}

bool GcRunner::mark_commit_data(const Hash& commit, GcMarkSet& marked,
                                MarkTraversal& traversal,
                                std::string& error) {
    std::vector<uint8_t> body;
    if (!store_.read_commit(commit, body)) {
        record_missing_hash(store_, commit, traversal.missing);
        error = "mark: commit object missing: " + hash_to_hex(commit);
        return false;
    }
    CommitData cd;
    if (!deserialize_commit(body, cd)) {
        error = "mark: corrupt commit: " + hash_to_hex(commit);
        return false;
    }
    return mark_tree_recursive(cd.tree_hash, marked, traversal, error);
}

bool GcRunner::mark_commit_chain(const Hash& head, const GcPolicy& policy,
                                 GcMarkSet& marked,
                                 std::vector<Hash>& retained_data,
                                 GcMarkSet& compacted_candidates,
                                 MarkTraversal& traversal,
                                 std::string& error,
                                 bool apply_keep_last_window) {
    // BFS over ALL parents: commit METADATA is always kept (label
    // resolution and history stay intact — spec Part 2). Data retention is
    // decided per commit below.
    std::vector<Hash> queue{head};
    std::set<std::string> seen;
    uint32_t first_parent_depth = 0;
    Hash first_parent_cursor = head;

    // Pass 1: which commits on the FIRST-PARENT chain are within keep_last?
    // A read/parse failure here breaks out of the window walk; the all-parent
    // BFS below re-reads every commit and fails closed (returns false with an
    // error) if one is genuinely missing, so breaking early is safe.
    std::unordered_set<Hash, GcHashHasher> keep_last_set;
    if (policy.keep_last.has_value() && apply_keep_last_window) {
        uint32_t n = std::max<uint32_t>(1, *policy.keep_last);
        while (first_parent_depth < n) {
            keep_last_set.insert(first_parent_cursor);
            first_parent_depth++;
            std::vector<uint8_t> body;
            if (!store_.read_commit(first_parent_cursor, body)) break;
            CommitData cd;
            if (!deserialize_commit(body, cd)) break;
            if (cd.parents.empty()) break;
            first_parent_cursor = cd.parents[0];
        }
    }

    while (!queue.empty()) {
        Hash cur = queue.back(); queue.pop_back();
        std::string hex = hash_to_hex(cur);
        if (!seen.insert(hex).second) continue;
        std::vector<uint8_t> body;
        if (!store_.read_commit(cur, body)) {
            record_missing_hash(store_, cur, traversal.missing);
            error = "mark: commit object missing: " + hex;
            return false;
        }
        CommitData cd;
        if (!deserialize_commit(body, cd)) {
            error = "mark: corrupt commit: " + hex;
            return false;
        }
        marked.insert(cur);                        // metadata always
        bool retain_data;
        if (!policy.keep_last.has_value()) {
            retain_data = true;                    // compaction disabled
        } else {
            // keep_last is a branch-head policy. State and runtime roots keep
            // their exact linked snapshot, but do not create an additional
            // N-commit retention window of their own.
            retain_data = (cur == head) ||
                          (apply_keep_last_window &&
                           keep_last_set.count(cur) != 0);
            for (auto& l : policy.keep_labels)
                if (cd.label == l) retain_data = true;
            // Compaction CANDIDATE only: a commit reached by BOTH the branch
            // BFS (compacted here) AND a state-record fs_commit link (whose
            // data IS retained) must NOT count as compacted. The final
            // tally is resolved at the end of mark() once retained_data is
            // fully populated, by subtracting state-retained commits.
            if (!retain_data) compacted_candidates.insert(cur);
        }
        if (retain_data) retained_data.push_back(cur);
        for (auto& p : cd.parents) queue.push_back(p);
    }
    return true;
}

bool GcRunner::mark_state_record(const Hash& id, const GcPolicy& policy,
                                 GcMarkSet& marked,
                                 std::vector<Hash>& retained_data,
                                 GcMarkSet& compacted_candidates,
                                 MarkTraversal& traversal,
                                 std::string& error) {
    if (!traversal.state_records.insert(id).second) return true;
    AgentStateRecord rec;
    std::string rerr;
    if (!read_agent_state_record(store_, id, rec, rerr)) {
        record_missing_hash(store_, id, traversal.missing);
        error = "mark: state record unreadable: " + hash_to_hex(id) + ": " + rerr;
        return false;
    }
    marked.insert(id);
    // payload_ref is marked separately for legacy records written before the
    // explicit dependency_hash list was persisted.
    if (!mark_required_hex_blob(rec.payload_ref, marked, error)) return false;
    for (const Hash& dependency : rec.dependency_hashes)
        marked.insert(dependency);
    if (!(rec.fs_commit == ZERO_HASH)) {
        if (!mark_commit_chain(rec.fs_commit, policy, marked, retained_data,
                               compacted_candidates, traversal, error,
                               false)) return false;
    }
    Hash next;
    if (!rec.union_state_id.empty()) {
        if (hex_to_hash_strict(rec.union_state_id, next)) {
            if (!mark_union_state(next, policy, marked, retained_data,
                                  compacted_candidates, traversal, error)) return false;
        } else { error = "mark: malformed union_state_id on " + hash_to_hex(id); return false; }
    }
    for (const std::string* pid : {&rec.parent_state_id, &rec.snapshot_base_state_id}) {
        if (pid->empty()) continue;
        if (hex_to_hash_strict(*pid, next)) {
            if (!mark_state_record(next, policy, marked, retained_data,
                                   compacted_candidates, traversal, error)) return false;
        } else { error = "mark: malformed state id on " + hash_to_hex(id); return false; }
    }
    return true;
}

bool GcRunner::mark_union_state(const Hash& id, const GcPolicy& policy,
                                GcMarkSet& marked,
                                std::vector<Hash>& retained_data,
                                GcMarkSet& compacted_candidates,
                                MarkTraversal& traversal,
                                std::string& error) {
    if (!traversal.union_states.insert(id).second) return true;
    UnionRuntimeState us;
    std::string rerr;
    if (!read_union_runtime_state(store_, id, us, rerr)) {
        record_missing_hash(store_, id, traversal.missing);
        error = "mark: union state unreadable: " + hash_to_hex(id) + ": " + rerr;
        return false;
    }
    marked.insert(id);
    if (!(us.fs_commit == ZERO_HASH)) {
        if (!mark_commit_chain(us.fs_commit, policy, marked, retained_data,
                               compacted_candidates, traversal, error,
                               false)) return false;
    }
    mark_opaque_cas_ref(us.command_ref, marked);
    mark_opaque_cas_ref(us.resource_manifest_ref, marked);
    Hash next;
    if (!us.agent_state_id.empty()) {
        if (hex_to_hash_strict(us.agent_state_id, next)) {
            if (!mark_state_record(next, policy, marked, retained_data,
                                   compacted_candidates, traversal, error)) return false;
        } else { error = "mark: malformed agent_state_id on " + hash_to_hex(id); return false; }
    }
    if (!us.parent_union_state_id.empty()) {
        if (hex_to_hash_strict(us.parent_union_state_id, next)) {
            if (!mark_union_state(next, policy, marked, retained_data,
                                  compacted_candidates, traversal, error)) return false;
        } else { error = "mark: malformed parent_union_state_id"; return false; }
    }
    return true;
}

bool GcRunner::mark(const GcLiveRoots& live, const GcPolicy& policy,
                    GcMarkSet& marked, GcResult& stats, std::string& error) {
    return mark_impl(live, policy, marked, stats, error, nullptr);
}

bool GcRunner::mark_impl(const GcLiveRoots& live, const GcPolicy& policy,
                         GcMarkSet& marked, GcResult& stats,
                         std::string& error, std::vector<Hash>* missing) {
    std::vector<Hash> retained_data;
    GcMarkSet compacted_candidates;
    MarkTraversal traversal;
    traversal.missing = missing;

    // 1. Branch refs.
    std::vector<std::string> ref_names;
    std::string refs_error;
    if (!refs_.list_refs(ref_names, refs_error)) {
        error = "mark: " + refs_error;
        return false;
    }
    for (const std::string& expected : live.expected_branch_refs) {
        if (!std::binary_search(ref_names.begin(), ref_names.end(), expected)) {
            error = "mark: expected branch ref missing: " + expected;
            return false;
        }
    }
    for (auto& name : ref_names) {
        Hash head;
        if (!refs_.read_ref(name, head)) {
            error = "mark: unreadable ref: " + name;
            return false;
        }
        stats.roots_branches++;
        if (!mark_commit_chain(head, policy, marked, retained_data,
                               compacted_candidates, traversal,
                               error)) return false;
    }

    // 2. Agent-state latest refs: <store>/state/latest/<agent>/<branch>.
    std::string latest_root = store_.root() + "/state/latest";
    std::error_code ec;
    fs::directory_iterator agents(latest_root, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "mark: enumerate state/latest failed: " + ec.message();
        return false;
    }
    ec.clear(); // a missing latest root is valid: no state has been appended
    for (; agents != fs::directory_iterator(); agents.increment(ec)) {
        if (ec) break;
        std::error_code type_error;
        bool is_dir = agents->is_directory(type_error);
        if (type_error) {
            error = "mark: inspect state/latest entry failed: " +
                    type_error.message();
            return false;
        }
        if (!is_dir) continue;

        std::error_code aec;
        fs::directory_iterator state_refs(agents->path(), aec);
        if (aec) {
            error = "mark: enumerate state/latest agent failed: " +
                    aec.message();
            return false;
        }
        for (; state_refs != fs::directory_iterator(); state_refs.increment(aec)) {
            if (aec) break;
            std::error_code ref_type_error;
            bool regular = state_refs->is_regular_file(ref_type_error);
            if (ref_type_error) {
                error = "mark: inspect state latest ref failed: " +
                        ref_type_error.message();
                return false;
            }
            if (!regular) continue;

            const fs::path ref_path = state_refs->path();
            std::ifstream in(ref_path);
            std::string hex;
            if (!(in >> hex)) {
                error = "mark: unreadable state latest ref: " + ref_path.string();
                return false;
            }
            std::string trailing;
            if (in >> trailing) {
                error = "mark: malformed state latest ref: " + ref_path.string();
                return false;
            }
            if (in.bad()) {
                error = "mark: unreadable state latest ref: " + ref_path.string();
                return false;
            }
            Hash id;
            if (!hex_to_hash_strict(hex, id)) {
                error = "mark: malformed state latest ref: " + ref_path.string();
                return false;
            }
            stats.roots_state_refs++;
            if (!mark_state_record(id, policy, marked, retained_data,
                                   compacted_candidates, traversal, error)) return false;
        }
        if (aec) {
            error = "mark: enumerate state/latest agent failed: " + aec.message();
            return false;
        }
    }
    if (ec) {
        error = "mark: enumerate state/latest failed: " + ec.message();
        return false;
    }

    // 3. Live runtime union states.
    for (auto& h : live.runtime_union_states) {
        stats.roots_runtime_states++;
        if (!mark_union_state(h, policy, marked, retained_data,
                              compacted_candidates, traversal, error)) return false;
    }

    // 4. Live daemon state.
    for (auto& h : live.wt_hashes) marked.insert(h);
    for (auto& h : live.fh_blob_hashes) marked.insert(h);
    for (auto& h : live.fh_pinned_commits) marked.insert(h);   // metadata only
    for (auto& h : live.pending) marked.insert(h);

    // 5. Data for retained commits (dedup via marked-tree short-circuit).
    for (auto& c : retained_data)
        if (!mark_commit_data(c, marked, traversal, error)) return false;

    // 6. Resolve compaction stats. A branch-BFS candidate is genuinely
    // compacted ONLY if its data is not retained through some other path
    // (e.g. a state-record / union-state fs_commit link). retained_data is
    // now complete, so subtract state-retained commits from the candidate
    // set: this avoids over-counting commits reached by both the branch
    // BFS and a state link.
    GcMarkSet retained_set(retained_data.begin(), retained_data.end());
    for (auto& c : compacted_candidates)
        if (!retained_set.count(c)) stats.compacted_commits++;

    return true;
}

GcResult GcRunner::run(const GcLiveRoots& live, const GcPolicy& policy) {
    GcResult r;
    auto t0 = std::chrono::steady_clock::now();
    // Capture the fence instant ONCE, up front: the sweep must never touch
    // an object whose mtime is within 2 seconds of this moment, because a
    // concurrent FUSE publisher may have written a fresh, not-yet-referenced
    // object. mtime + 2 >= mark_start => protected.
    const int64_t mark_start = static_cast<int64_t>(std::time(nullptr));

    std::string error;
    GcMarkSet marked;
    if (!mark(live, policy, marked, r, error)) {   // fail closed: no sweep
        r.error = error;
        return r;
    }

    std::string enum_err;
    bool enum_ok = store_.for_each_object(
        [&](const Hash& h, uint64_t size, int64_t mtime) {
            if (marked.count(h)) {
                r.marked_objects++;
                r.marked_bytes += size;
                return;
            }
            // Age fence (global constraint): never touch fresh objects — a
            // concurrent FUSE publish may not be referenced anywhere yet.
            if (mtime + 2 >= mark_start) return;
            if (policy.dry_run) {
                r.swept_objects++;
                r.swept_bytes += size;
                return;
            }

            std::string remove_error;
            ObjectStore::RemoveResult removed =
                store_.remove_object_if_older_than(h, mark_start - 2,
                                                   remove_error);
            if (removed == ObjectStore::RemoveResult::Removed) {
                r.swept_objects++;
                r.swept_bytes += size;
            } else if (removed == ObjectStore::RemoveResult::Error) {
                r.sweep_errors++;
                std::fprintf(stderr, "agentvfs: gc: %s\n", remove_error.c_str());
                if (r.error.size() < 2000) {
                    if (!r.error.empty()) r.error += "; ";
                    r.error += remove_error;
                }
            }
        }, enum_err);
    if (!enum_ok) {
        if (!r.error.empty()) r.error += "; ";
        r.error += enum_err;
    }

    // The blob-size cache assumes objects are never deleted; that holds only
    // between sweeps, so invalidate it after a real sweep deleted something.
    if (!policy.dry_run && r.swept_objects > 0) store_.clear_size_cache();
    r.duration_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    r.ok = enum_ok && r.sweep_errors == 0;
    return r;
}

GcResult GcRunner::verify(const GcLiveRoots& live, const GcPolicy& policy) {
    GcResult r;
    std::string error;
    GcMarkSet marked;
    if (!mark_impl(live, policy, marked, r, error, &r.missing)) {
        std::sort(r.missing.begin(), r.missing.end());
        r.missing.erase(std::unique(r.missing.begin(), r.missing.end()),
                        r.missing.end());
        r.missing_objects = r.missing.size();
        r.error = error;
        return r;
    }
    for (auto& h : marked) {
        r.marked_objects++;
        if (!store_.object_exists(h)) {
            r.missing_objects++;
            r.missing.push_back(h);
            // Cap the error blob so a pathological mark set cannot exhaust
            // memory in the control response.
            if (r.error.size() < 2000)
                r.error += (r.error.empty() ? "missing: " : ", ") + hash_to_hex(h);
        }
    }
    std::sort(r.missing.begin(), r.missing.end());
    r.ok = (r.missing_objects == 0);
    return r;
}

} // namespace cas
