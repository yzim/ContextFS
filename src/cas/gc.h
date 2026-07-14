#pragma once
#include "hash.h"
#include "object_store.h"
#include "refs.h"
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace cas {

struct GcHashHasher {
    size_t operator()(const Hash& h) const noexcept {
        size_t v; std::memcpy(&v, h.data(), sizeof(v)); return v;
    }
};
using GcMarkSet = std::unordered_set<Hash, GcHashHasher>;

struct GcPolicy {
    std::optional<uint32_t> keep_last;    // compaction ON only when set
    std::vector<std::string> keep_labels; // retain commits with these exact labels
    bool dry_run = false;
};

// Live daemon state collected under the GC locks (Daemon::run_gc builds
// this; tests may build it by hand).
struct GcLiveRoots {
    // Names observed in the daemon's in-memory branch map. GC requires each
    // corresponding durable ref to be present, so disappearance cannot look
    // like an intentionally smaller root set.
    std::vector<std::string> expected_branch_refs;
    std::vector<Hash> wt_hashes;          // every branch, both layers, != ZERO_HASH
    std::vector<Hash> fh_blob_hashes;     // FhState::base_blob per open fh
    std::vector<Hash> fh_pinned_commits;  // commit blobs marked (metadata only)
    std::vector<Hash> pending;            // ObjectStore::pending_snapshot()
    std::vector<Hash> runtime_union_states; // live supervisor union-state ids
};

struct GcResult {
    bool ok = false;
    std::string error;
    uint64_t roots_branches = 0, roots_state_refs = 0, roots_runtime_states = 0;
    uint64_t marked_objects = 0, marked_bytes = 0;
    uint64_t swept_objects = 0, swept_bytes = 0;
    uint64_t sweep_errors = 0;
    uint64_t compacted_commits = 0;
    uint64_t missing_objects = 0;         // verify only
    std::vector<Hash> missing;            // verify only
    uint64_t duration_ms = 0;
};

class GcRunner {
public:
    GcRunner(ObjectStore& store, Refs& refs);
    // Mark phase. Fail-closed: any read error aborts (returns false, error
    // set) and the caller must not sweep.
    bool mark(const GcLiveRoots& live, const GcPolicy& policy,
              GcMarkSet& marked, GcResult& stats, std::string& error);
    // mark + sweep (age-fenced) + clear_size_cache. dry_run: mark only,
    // swept_* report what WOULD be deleted.
    GcResult run(const GcLiveRoots& live, const GcPolicy& policy);
    // mark with the same policy, then check every marked object exists.
    GcResult verify(const GcLiveRoots& live, const GcPolicy& policy);
private:
    struct MarkTraversal {
        GcMarkSet trees;
        GcMarkSet state_records;
        GcMarkSet union_states;
        std::vector<Hash>* missing = nullptr;
    };

    bool mark_impl(const GcLiveRoots& live, const GcPolicy& policy,
                   GcMarkSet& marked, GcResult& stats, std::string& error,
                   std::vector<Hash>* missing);

    bool mark_commit_chain(const Hash& head, const GcPolicy& policy,
                           GcMarkSet& marked, std::vector<Hash>& retained_data,
                           GcMarkSet& compacted_candidates,
                           MarkTraversal& traversal, std::string& error,
                           bool apply_keep_last_window = true);
    bool mark_tree_recursive(const Hash& tree, GcMarkSet& marked,
                             MarkTraversal& traversal, std::string& error);
    bool mark_commit_data(const Hash& commit, GcMarkSet& marked,
                          MarkTraversal& traversal, std::string& error);
    bool mark_state_record(const Hash& id, const GcPolicy& policy,
                           GcMarkSet& marked, std::vector<Hash>& retained_data,
                           GcMarkSet& compacted_candidates,
                           MarkTraversal& traversal, std::string& error);
    bool mark_union_state(const Hash& id, const GcPolicy& policy,
                          GcMarkSet& marked, std::vector<Hash>& retained_data,
                          GcMarkSet& compacted_candidates,
                          MarkTraversal& traversal, std::string& error);
    ObjectStore& store_;
    Refs& refs_;
};

} // namespace cas
