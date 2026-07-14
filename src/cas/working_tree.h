#pragma once
#include "hash.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cas {

enum class EntryKind : uint8_t { Blob = 0, Tree = 1, Symlink = 2, Deleted = 3 };

struct WorkingTreeEntry {
    EntryKind kind;
    Hash hash;
    uint32_t mode;
};

struct WorkingTreeMemoryStats {
    size_t base_entries = 0;
    int64_t base_shared_by = 0;
    size_t delta_entries = 0;
    size_t delta_tombstones = 0;
};

// Two-level copy-on-write path map (2026-07-13 mem-and-gc design).
//
//   base_  — immutable snapshot shared across branches via shared_ptr.
//            May contain Deleted whiteouts (rebuilds from CAS trees keep
//            them). Never mutated after publication.
//   delta_ — this tree's private overlay. An entry here (including
//            Deleted) shadows the base entry for the same path.
//
// clone() shares base_ and copies delta_, so branch creation costs
// O(churn since the last freeze point) instead of O(tree).
//
// base_authoritative_ gates tombstone hygiene: while the bootstrap walk is
// still filling the tree, a lookup miss falls through to the live source
// directory, so remove() must always leave a whiteout. Once the base is
// authoritative (bootstrap walk folded, or rebuilt from a commit),
// removing a path absent from both the base and live source simply erases
// the delta entry.
class WorkingTree {
public:
    using EntryMap = std::map<std::string, WorkingTreeEntry>;

    WorkingTree() = default;
    WorkingTree(WorkingTree&& other) noexcept;
    WorkingTree& operator=(WorkingTree&& other) noexcept;
    WorkingTree(const WorkingTree&) = delete;
    WorkingTree& operator=(const WorkingTree&) = delete;

    std::optional<WorkingTreeEntry> lookup(const std::string& path) const;
    std::optional<WorkingTreeEntry> lookup_raw(const std::string& path) const;
    void insert(const std::string& path, const WorkingTreeEntry& entry);
    // Records that the live source directory still contains this path. The
    // marker survives overlay updates so a later unlink/rename leaves a
    // whiteout even when the path was discovered after base publication.
    void insert_source(const std::string& path, const WorkingTreeEntry& entry);
    void remove(const std::string& path);
    void rename_entry(const std::string& old_path, const std::string& new_path);
    void rename_dir(const std::string& old_prefix, const std::string& new_prefix);

    std::vector<std::pair<std::string, WorkingTreeEntry>> list_dir(const std::string& dir_path) const;

    void clear();
    void for_each(const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const;
    void for_each_including_deleted(
        const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const;
    size_t size() const;

    // Shares base_, copies delta_, source-origin markers, and authority.
    WorkingTree clone() const;

    // Publishes `entries` as the new immutable authoritative base; the
    // delta is cleared. Used by rebuild_working_tree
    // (rollback / startup-from-ref) and tests.
    void set_base(EntryMap&& entries);

    // Folds delta_ into the base and publishes the result as the new
    // authoritative base. O(1) move when the base is empty (fresh mount);
    // O(base + delta) merge otherwise (daemon restart where a rebuilt base
    // was extended by the walk). Called once at bootstrap-walk completion.
    void fold_into_base();

    // A source walk makes base misses unsafe until the walk completes and
    // fold_into_base() atomically republishes an authoritative snapshot.
    void begin_source_walk();

    bool base_authoritative() const;

    // Coherent stats.memory snapshot sampled under one WorkingTree lock.
    WorkingTreeMemoryStats memory_stats() const;

    // Legacy single-field accessors retained for source compatibility.
    size_t base_entry_count() const;        // includes whiteouts
    long   base_shared_count() const;       // shared_ptr use_count; 0 = no base
    size_t delta_entry_count() const;       // includes tombstones
    size_t delta_tombstone_count() const;

    std::mutex& mutex() { return mu_; }

private:
    std::optional<WorkingTreeEntry> lookup_raw_locked(const std::string& path) const;
    void remove_locked(const std::string& path);
    // Merged ordered iteration over base_ and delta_ (delta wins per key).
    void merged_for_each_locked(
        const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const;

    mutable std::mutex mu_;
    std::shared_ptr<const EntryMap> base_;
    EntryMap delta_;
    // Paths known to remain present in source_root but not necessarily in
    // base_. Markers are folded away once their entries become base content.
    std::set<std::string> source_origin_paths_;
    bool base_authoritative_ = false;
};

} // namespace cas
