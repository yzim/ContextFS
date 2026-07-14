#pragma once
#include "hash.h"
#include "object_store.h"
#include "working_tree.h"
#include <string>
#include <vector>

namespace cas {

std::vector<uint8_t> serialize_tree_entries(
    const std::vector<std::pair<std::string, WorkingTreeEntry>>& children);

bool deserialize_tree_entries(
    const std::vector<uint8_t>& body,
    std::vector<std::tuple<std::string, uint32_t, EntryKind, Hash>>& out);

Hash serialize_working_tree(
    const WorkingTree& wt,
    ObjectStore& store,
    std::vector<Hash>& written_hashes,
    std::vector<Hash>* referenced_leaf_hashes = nullptr,
    std::string* error = nullptr);

// Rebuilds a WorkingTree from a serialized root tree. On success the
// rebuilt map is published via WorkingTree::set_base (authoritative base,
// empty delta). On ANY failure returns false with `wt` untouched; when a
// a tree object is genuinely absent, *error starts with "tree object missing"
// (rollback maps only that case to the retention-compaction error). Existing
// but unreadable or corrupt objects use a distinct error.
bool rebuild_working_tree(
    const Hash& root_tree,
    ObjectStore& store,
    WorkingTree& wt,
    std::string* error = nullptr);

} // namespace cas
