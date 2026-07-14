#pragma once
#include "working_tree.h"
#include <cstdint>
#include <mutex>
#include <string>

namespace cas {

// BranchContext holds a std::mutex, so it is non-copyable and non-movable.
// The Daemon owns instances through std::shared_ptr so FUSE ops can keep
// a branch alive past its deletion from the branch map; tests may
// stack-allocate directly for isolation checks.
struct BranchContext {
    uint32_t    branch_id;
    std::string name;
    WorkingTree wt;
    std::mutex  checkpoint_mu;
    // Guarded by checkpoint_mu. Set before a deleted branch leaves the daemon
    // map so operations that resolved its shared_ptr earlier fail stale rather
    // than mutating a detached tree or creating a new live file handle.
    bool retired = false;

    BranchContext(uint32_t id, std::string n)
        : branch_id(id), name(std::move(n)) {}

    BranchContext(uint32_t id, std::string n, WorkingTree&& tree)
        : branch_id(id), name(std::move(n)), wt(std::move(tree)) {}

    BranchContext(const BranchContext&) = delete;
    BranchContext& operator=(const BranchContext&) = delete;
    BranchContext(BranchContext&&) = delete;
    BranchContext& operator=(BranchContext&&) = delete;
};

} // namespace cas
