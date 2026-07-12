#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cas {

// Process identifier. Wide enough to hold any platform's native PID type
// (Linux pid_t is signed 32-bit; macOS same; Windows DWORD is 32-bit).
// Defined as int32_t rather than pid_t so this header is portable: cas_core
// must compile on MSVC, which does not define pid_t.
using Pid = int32_t;

class BranchRouter {
public:
    struct Hooks {
        std::function<uint64_t(const std::string&)> cgroup_id;
        std::function<std::string(Pid)> read_cgroup;
        std::function<uint64_t()> fence_generation;
        std::function<bool(Pid)> fence_track;
    };

    BranchRouter();
    explicit BranchRouter(Hooks hooks);

    // Returns true if cgroup_path resolves to a valid cgroup v2 directory
    // and the mapping was recorded; false otherwise (nonexistent path,
    // stat failure, not a directory). A false return should be surfaced
    // to the caller — silently mapping a bogus cgroup would let every
    // FUSE op from its would-be members fall through to main.
    //
    // On non-Linux platforms this always returns false: there is no
    // cgroup namespace to register against, so every PID resolves to the
    // main branch.
    bool register_cgroup(const std::string& cgroup_path, uint32_t branch_id);

    bool unregister_cgroup(const std::string& cgroup_path);
    void invalidate_branch(uint32_t branch_id);

    // External-deletion eviction, driven by CgroupWatch: the registered
    // cgroup identified by cg_id was rmdir'd outside the control plane.
    // Erases the registration (so a later recycled inode number cannot
    // resurrect it) and invalidates the routing caches.
    void evict_cgroup_id(uint64_t cg_id);

    // With `on` (the default) every Phase-1 resolve stats the caller's
    // leaf cgroup and accepts a memoized entry only when the inode still
    // matches — synchronous delete/recreate detection. The daemon turns
    // this off while a cgroup delete watch is active, in which case
    // recreate detection arrives as evict_cgroup_id() instead and warm
    // resolves cost one procfs membership read and no stat.
    void set_leaf_revalidation(bool on);

    // Returns 0 on non-Linux (no cgroup-based routing) and on Linux when
    // no mapping is found.
    uint32_t resolve(Pid pid);

    // Wire a kernel-side invalidation fence (see routing_fence.h). The
    // fence is considered active when the generation hook is set. Must be
    // called before the FUSE loop starts serving requests: it mutates
    // hooks_ without holding mu_.
    void attach_fence(std::function<uint64_t()> generation,
                      std::function<bool(Pid)> track);

    bool has_cgroup_for_branch(uint32_t branch_id) const;

private:
    static uint64_t cgroup_id_from_path(const std::string& path);
    static std::string read_proc_cgroup(Pid pid);

    uint64_t cgroup_id(const std::string& path) const;
    std::string proc_cgroup(Pid pid) const;
    std::vector<uint64_t> cgroup_path_ids(const std::string& cgroup_abs_path,
                                          uint64_t leaf_cgroup_id) const;
    uint32_t branch_for_cgroup_ids(const std::vector<uint64_t>& ids) const;

    mutable std::mutex mu_;
    std::unordered_map<uint64_t, uint32_t> cgroup_branch_map_;
    // Memoized branch lookup keyed by the /proc-relative cgroup path. The
    // stored leaf id supports delete/recreate detection: it is checked on
    // every Phase-1 resolve when revalidate_leaf_ is set, and kept fresh
    // by the miss-path walk either way (so revalidation can be re-enabled
    // at any time, e.g. when a delete watch fails mid-run).
    struct PathEntry {
        uint64_t leaf_cgroup_id;
        uint32_t branch_id;
    };
    static constexpr size_t kPathCacheCap = 1024;
    std::unordered_map<std::string, PathEntry> path_branch_cache_;
    // Pid cache used ONLY while the fence reports an unchanged generation.
    // Strictness: entries are written with a generation sampled after the
    // pid was tracked, so any later migration/exit bumps the counter and
    // the stale entry can never satisfy the fast path.
    struct FenceEntry {
        uint32_t branch_id;
        uint64_t generation;
    };
    static constexpr size_t kFenceCacheCap = 4096;
    std::unordered_map<Pid, FenceEntry> pid_fence_cache_;
    void fence_cache_store(Pid pid, uint32_t branch_id, uint64_t gen);
    uint64_t generation_ = 0;
    // Atomic: flipped by the daemon/control threads (watch start/failure)
    // while FUSE threads read it on every resolve.
    std::atomic<bool> revalidate_leaf_{true};
    Hooks hooks_;
};

} // namespace cas
