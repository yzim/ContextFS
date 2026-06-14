#pragma once
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
        std::function<uint64_t(Pid)> read_starttime;
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

    // Returns 0 on non-Linux (no cgroup-based routing) and on Linux when
    // no mapping is found.
    uint32_t resolve(Pid pid);

    bool has_cgroup_for_branch(uint32_t branch_id) const;

private:
    static uint64_t cgroup_id_from_path(const std::string& path);
    static std::string read_proc_cgroup(Pid pid);
    // Read field 22 (start-time-in-clock-ticks since boot) from
    // /proc/<pid>/stat. Returns 0 on failure. Used to detect PID reuse
    // without having to invalidate the cache on every resolve.
    static uint64_t read_proc_starttime(Pid pid);

    struct CachedMapping {
        uint32_t branch_id;
        uint64_t starttime;  // process start time; 0 means not-validated
    };

    uint64_t cgroup_id(const std::string& path) const;
    std::string proc_cgroup(Pid pid) const;
    uint64_t proc_starttime(Pid pid) const;
    std::vector<uint64_t> cgroup_path_ids(const std::string& cgroup_abs_path) const;
    uint32_t branch_for_cgroup_ids(const std::vector<uint64_t>& ids) const;

    mutable std::mutex mu_;
    std::unordered_map<uint64_t, uint32_t> cgroup_branch_map_;
    std::unordered_map<Pid, CachedMapping> pid_cache_;
    uint64_t generation_ = 0;
    Hooks hooks_;
};

} // namespace cas
